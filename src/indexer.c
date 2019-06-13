/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#include <string.h>
#include <limits.h>
#include <rmutil/alloc.h>
#include <rmutil/vector.h>

#include "consts.h"
#include "indexer.h"

RedisModuleDict *labelsIndex;

#define KV_PREFIX "__index_%s=%s"
#define K_PREFIX "__key_index_%s"


void IndexInit() {
    labelsIndex = RedisModule_CreateDict(NULL);
}

int parseLabel(RedisModuleCtx *ctx, RedisModuleString *label, QueryPredicate *retQuery, const char *separator) {
    char *token;
    char *iter_ptr;
    size_t _s;
    const char *labelRaw = RedisModule_StringPtrLen(label, &_s);
    char *labelstr = RedisModule_PoolAlloc(ctx, _s + 1);
    labelstr[_s] = '\0';
    strncpy(labelstr, labelRaw, _s);

    // Extract key
    token = strtok_r(labelstr, separator, &iter_ptr);
    if (token == NULL) {
        return TSDB_ERROR;
    }
    retQuery->key = RedisModule_CreateString(ctx, token, strlen(token));

    // Extract value
    token = strtok_r(NULL, separator, &iter_ptr);
    if (strcmp(separator, "=(") == 0) {
        if (token == NULL) {
            return TSDB_ERROR;
        }

        if (token[strlen(token) - 1] == ')') {
            token[strlen(token) - 1] = '\0'; // remove closing parentheses
        } else {
            return TSDB_ERROR;
        }

        int filterCount = 1;
        for (int i=0; token[i]!='\0'; i++) {
            if (token[i] == ',') {
                filterCount++;
            }
        }
        retQuery->valueListCnt = filterCount;
        retQuery->valuesList = RedisModule_PoolAlloc(ctx, filterCount * sizeof(RedisModuleString*));

        char* subToken = strtok_r(token, ",", &iter_ptr);
        for (int i=0; i < filterCount; i++) {
            retQuery->valuesList[i] = RedisModule_CreateStringPrintf(ctx, subToken);
            subToken = strtok_r(NULL, ",", &iter_ptr);
        }
    } else if (token != NULL) {
        retQuery->valueListCnt = 1;
        retQuery->valuesList = RedisModule_PoolAlloc(ctx, sizeof(RedisModuleString*));
        retQuery->valuesList[0] = RedisModule_CreateString(ctx, token, strlen(token));
    } else {
        retQuery->valuesList = NULL;
        retQuery->valueListCnt = 0;
    }
    return TSDB_OK;
}

int CountPredicateType(QueryPredicate *queries, size_t query_count, PredicateType type) {
    int count = 0;
    for (int i=0; i<query_count; i++) {
        if (queries[i].type == type) {
        count++;
        }
    }
    return count;
}

void indexUnderKey(const char *op, RedisModuleString *key, RedisModuleString *ts_key) {
    int nokey = 0;
    RedisModuleDict *leaf = RedisModule_DictGet(labelsIndex, key, &nokey);
    if (nokey) {
        leaf = RedisModule_CreateDict(NULL);
        RedisModule_DictSet(labelsIndex, key, leaf);
    }

    if (strcmp(op, "SADD") == 0) {
        RedisModule_DictSet(leaf, ts_key, NULL);
    } else if (strcmp(op, "SREM") == 0) {
        RedisModule_DictDel(leaf, ts_key, NULL);
    }
}

void IndexOperation(RedisModuleCtx *ctx, const char *op, RedisModuleString *ts_key, Label *labels, size_t labels_count) {
    const char *key_string, *value_string;
    for (int i=0; i<labels_count; i++) {
        size_t _s;
        key_string = RedisModule_StringPtrLen(labels[i].key, &_s);
        value_string = RedisModule_StringPtrLen(labels[i].value, &_s);
        RedisModuleString *indexed_key_value = RedisModule_CreateStringPrintf(ctx, KV_PREFIX,
                                                                              key_string,
                                                                              value_string);
        RedisModuleString *indexed_key = RedisModule_CreateStringPrintf(ctx, K_PREFIX,
                                                                        key_string);

        indexUnderKey(op, indexed_key_value, ts_key);
        indexUnderKey(op, indexed_key, ts_key);
    }
}

void IndexMetric(RedisModuleCtx *ctx, RedisModuleString *ts_key, Label *labels, size_t labels_count) {
    IndexOperation(ctx, "SADD", ts_key, labels, labels_count);
}

void RemoveIndexedMetric(RedisModuleCtx *ctx, RedisModuleString *ts_key, Label *labels, size_t labels_count) {
    IndexOperation(ctx, "SREM", ts_key, labels, labels_count);
}

void _union(RedisModuleCtx *ctx, RedisModuleDict *dest, RedisModuleDict *src) {
    /*
    * Copy all elements from src to dest
    */
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(src, "^", NULL, 0);
    RedisModuleString *currentKey;
    while ((currentKey = RedisModule_DictNext(ctx, iter, NULL)) != NULL) {
        RedisModule_DictSet(dest, currentKey, (void *) 1);
    }
    RedisModule_DictIteratorStop(iter);
}

void _intersect(RedisModuleCtx *ctx, RedisModuleDict *left, RedisModuleDict *right) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(left, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    while((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        int doesNotExist = 0;
        RedisModule_DictGetC(right, currentKey, currentKeyLen, &doesNotExist);
        if (doesNotExist == 0) {
            continue;
        }
        RedisModule_DictDelC(left, currentKey, currentKeyLen, NULL);
        RedisModule_DictIteratorReseekC(iter, ">", currentKey, currentKeyLen);
    }
    RedisModule_DictIteratorStop(iter);
}

void _difference(RedisModuleCtx *ctx, RedisModuleDict *left, RedisModuleDict *right) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(right, "^", NULL, 0);

    char *currentKey;
    size_t currentKeyLen;
    while((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        int doesNotExist = 0;
        RedisModule_DictGetC(left, currentKey, currentKeyLen, &doesNotExist);
        if (doesNotExist == 1) {
            continue;
        }
        RedisModule_DictDelC(left, currentKey, currentKeyLen, NULL);
        RedisModule_DictIteratorReseekC(iter, ">", currentKey, currentKeyLen);
    }
    RedisModule_DictIteratorStop(iter);
}

RedisModuleDict * GetPredicateKeysDict(RedisModuleCtx *ctx, QueryPredicate *predicate) {
    /*
     * Return the dictionary of all the keys that match the predicate.
     */
    RedisModuleDict *currentLeaf = NULL;
    RedisModuleString *index_key;
    size_t _s;
    const char *key = RedisModule_StringPtrLen(predicate->key, &_s);
    const char *value;

    int nokey;

    if (predicate->valuesList == 0) {
        index_key = RedisModule_CreateStringPrintf(ctx, K_PREFIX,
                                                   RedisModule_StringPtrLen(predicate->key, &_s));
        currentLeaf = RedisModule_DictGet(labelsIndex, index_key, &nokey);
    } else { // one or more entries
        RedisModuleDict *singleEntryLeaf;

        for (int i = 0; i < predicate->valueListCnt; i++) {
            value = RedisModule_StringPtrLen(predicate->valuesList[i], &_s);
            index_key = RedisModule_CreateStringPrintf(ctx, KV_PREFIX, key, value);
            singleEntryLeaf = RedisModule_DictGet(labelsIndex, index_key, &nokey);
            if (currentLeaf == NULL) {
                currentLeaf = singleEntryLeaf;
            } else if (singleEntryLeaf != NULL){
                _union(ctx, currentLeaf, singleEntryLeaf);
            }
        }
    }
    return currentLeaf;
}

RedisModuleDict * QueryIndexPredicate(RedisModuleCtx *ctx, QueryPredicate *predicate,
        RedisModuleDict *prevResults, int createResultDict) {
    RedisModuleDict *localResult = RedisModule_CreateDict(ctx);
    RedisModuleDict *currentLeaf;

    currentLeaf = GetPredicateKeysDict(ctx, predicate);

    if (currentLeaf != NULL) {
        // Copy everything to new dict only in case this is the first predicate (and there is more than one predicate).
        // In the next iteration, when prevResults is no longer NULL, there is no need to copy again. We can work on
        // currentLeaf, since only the left dict is being changed during intersection / difference.
        if (createResultDict && prevResults == NULL) {
            RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(currentLeaf, "^", NULL, 0);
            RedisModuleString *currentKey;
            while((currentKey = RedisModule_DictNext(ctx, iter, NULL)) != NULL) {
                RedisModule_DictSet(localResult, currentKey, (void *)1);
            }
            RedisModule_DictIteratorStop(iter);
        }
        else {
            localResult = currentLeaf;
        }
    }

    if (prevResults != NULL) {
        if (predicate->type == EQ || predicate->type == CONTAINS) {
            _intersect(ctx, prevResults, localResult);
        } else  if (predicate->type == NCONTAINS) {
            _difference(ctx, prevResults, localResult);
        } else if (predicate->type == NEQ){
            _difference(ctx, prevResults, localResult);
        } else if (predicate->type == FILTER_LIST) {
            _union(ctx, prevResults, localResult);
        }
        return prevResults;
    } else if (predicate->type == EQ || predicate->type == CONTAINS || predicate->type == FILTER_LIST) {
        return localResult;
    } else {
        return prevResults; // always NULL
    }
}

void PromoteSmallestPredicateToFront(RedisModuleCtx *ctx, QueryPredicate *index_predicate, size_t predicate_count) {
    /*
     * Find the predicate that has the minimal amount of keys that match to it, and move it to the beginning of the
     * predicate list so we will start our calculation from the smallest predicate. This is an optimization, so we will
     * copy the smallest dict possible.
     */
    if (predicate_count > 1) {
        int minIndex = 0;
        unsigned int minDictSize = UINT_MAX;
        for (int i = 0; i < predicate_count; i++) {
            RedisModuleDict *currentPredicateKeys = GetPredicateKeysDict(ctx, &index_predicate[i]);
            int currentDictSize =  (currentPredicateKeys != NULL) ? RedisModule_DictSize(currentPredicateKeys): 0;
            if (currentDictSize < minDictSize) {
                minIndex = i;
                minDictSize = currentDictSize;
            }
        }

        // switch between the minimal predicate and the predicate in the first place
        if (minIndex != 0) {
            QueryPredicate temp = index_predicate[minIndex];
            index_predicate[minIndex] = index_predicate[0];
            index_predicate[0] = temp;
        }
    }
}
RedisModuleDict * QueryIndex(RedisModuleCtx *ctx, QueryPredicate *index_predicate, size_t predicate_count) {
    RedisModuleDict *result = NULL;
    int shouldCreateNewDict = (predicate_count > 1);

    PromoteSmallestPredicateToFront(ctx, index_predicate, predicate_count);

    // EQ or Contains
    for (int i=0; i < predicate_count; i++) {
        if (index_predicate[i].type == EQ || index_predicate[i].type == CONTAINS || index_predicate[i].type == FILTER_LIST) {
            result = QueryIndexPredicate(ctx, &index_predicate[i], result, shouldCreateNewDict);
            if (result == NULL) {
                return RedisModule_CreateDict(ctx);
            }
        }
    }

    // The next two types of queries are reducers so we run them after the matcher
    // NCONTAINS or NEQ
    for (int i=0; i < predicate_count; i++) {
        if (index_predicate[i].type == NCONTAINS || index_predicate[i].type == NEQ) {
            result = QueryIndexPredicate(ctx, &index_predicate[i], result, shouldCreateNewDict);
            if (result == NULL) {
                return RedisModule_CreateDict(ctx);
            }
        }
    }

    if (result == NULL) {
        return RedisModule_CreateDict(ctx);
    }
    return result;
}
