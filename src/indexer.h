/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef INDEXER_H
#define INDEXER_H

#include "redismodule.h"

#include <sys/types.h>

typedef struct
{
    RedisModuleString *key;
    RedisModuleString *value;
} Label;

typedef enum
{
    EQ,
    NEQ,
    // Contains a label
    CONTAINS,
    // Not Contains a label
    NCONTAINS,
    LIST_MATCH,    // List of matching predicates
    LIST_NOTMATCH, // List of non-matching predicates
    // REQ,
    // NREQ
} PredicateType;

typedef struct QueryPredicate
{
    PredicateType type;
    RedisModuleString *key;
    RedisModuleString **valuesList;
    size_t valueListCount;
} QueryPredicate;

typedef struct QueryPredicateList
{
    QueryPredicate *list;
    size_t count;
    size_t ref;
} QueryPredicateList;

int parsePredicate(RedisModuleCtx *ctx,
                   RedisModuleString *label,
                   QueryPredicate *retQuery,
                   const char *separator);
void QueryPredicate_Free(QueryPredicate *predicate, size_t count);
void QueryPredicateList_Free(QueryPredicateList *list);

void IndexInit();
void FreeLabels(void *value, size_t labelsCount);
void IndexMetric(RedisModuleCtx *ctx,
                 RedisModuleString *ts_key,
                 Label *labels,
                 size_t labels_count);
void RemoveIndexedMetric(RedisModuleCtx *ctx,
                         RedisModuleString *ts_key,
                         Label *labels,
                         size_t labels_count);
RedisModuleDict *QueryIndex(RedisModuleCtx *ctx,
                            QueryPredicate *index_predicate,
                            size_t predicate_count);

int CountPredicateType(QueryPredicateList *queries, PredicateType type);
#endif
