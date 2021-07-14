/*
 * Copyright 2018-2021 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "resultset.h"

#include "indexer.h"
#include "redismodule.h"
#include "reply.h"
#include "series_iterator.h"
#include "string.h"
#include "tsdb.h"

#include "rmutil/alloc.h"

struct TS_ResultSet
{
    RedisModuleDict *groups;
    char *labelkey;
};

struct TS_GroupList
{
    char *labelValue;
    size_t count;
    Series **list;
};

TS_GroupList *GroupList_Create();

void GroupList_Free(TS_GroupList *g);

void GroupList_ApplyReducer(TS_GroupList *group,
                            char *labelKey,
                            RangeArgs *args,
                            MultiSeriesReduceOp reducerOp,
                            bool reverse);

void GroupList_ReplyResultSet(RedisModuleCtx *ctx,
                              TS_GroupList *group,
                              bool withlabels,
                              RedisModuleString *limitLabels[],
                              ushort limitLabelsSize,
                              RangeArgs *args,
                              bool reverse);

void FreeTempSeries(Series *s) {
    if (!s)
        return;
    RedisModule_FreeString(NULL, s->keyName);
    if (s->chunks) {
        RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(s->chunks, "^", NULL, 0);
        Chunk_t *currentChunk;
        while (RedisModule_DictNextC(iter, NULL, (void **)&currentChunk) != NULL) {
            s->funcs->FreeChunk(currentChunk);
        }
        RedisModule_DictIteratorStop(iter);
        RedisModule_FreeDict(NULL, s->chunks);
    }
    if (s->labels) {
        FreeLabels(s->labels, s->labelsCount);
    }
    free(s);
}

TS_GroupList *GroupList_Create() {
    TS_GroupList *g = (TS_GroupList *)malloc(sizeof(TS_GroupList));
    g->count = 0;
    g->labelValue = NULL;
    g->list = NULL;
    return g;
}

void GroupList_Free(TS_GroupList *groupList) {
    for (int i = 0; i < groupList->count; i++) {
        FreeTempSeries(groupList->list[i]);
    }
    free(groupList->labelValue);
    if (groupList->list)
        free(groupList->list);
    free(groupList);
}

int GroupList_AddSerie(TS_GroupList *g, Series *serie, const char *name) {
    if (g->list == NULL) {
        g->list = (Series **)malloc(sizeof(Series *));
    } else {
        g->list = (Series **)realloc(g->list, sizeof(Series *) * (g->count + 1));
    }
    g->list[g->count] = serie;
    g->count++;
    return REDISMODULE_OK;
}

void GroupList_ReplyResultSet(RedisModuleCtx *ctx,
                              TS_GroupList *group,
                              bool withlabels,
                              RedisModuleString *limitLabels[],
                              ushort limitLabelsSize,
                              RangeArgs *args,
                              bool rev) {
    for (int i = 0; i < group->count; i++) {
        ReplySeriesArrayPos(
            ctx, group->list[i], withlabels, limitLabels, limitLabelsSize, args, rev);
    }
}

Label *createReducedSeriesLabels(char *labelKey, char *labelValue, MultiSeriesReduceOp reducerOp) {
    // Labels:
    // <label>=<groupbyvalue>
    // __reducer__=<reducer>
    // __source__=key1,key2,key3
    char *reducer_str = NULL;
    switch (reducerOp) {
        case MultiSeriesReduceOp_Max:
            reducer_str = "max";
            break;
        case MultiSeriesReduceOp_Min:
            reducer_str = "min";
            break;
        case MultiSeriesReduceOp_Sum:
            reducer_str = "sum";
            break;
    }
    Label *labels = malloc(sizeof(Label) * 3);
    labels[0].key = RedisModule_CreateStringPrintf(NULL, "%s", labelKey);
    labels[0].value = RedisModule_CreateStringPrintf(NULL, "%s", labelValue);
    labels[1].key = RedisModule_CreateStringPrintf(NULL, "__reducer__");
    labels[1].value = RedisModule_CreateString(NULL, reducer_str, strlen(reducer_str));
    labels[2].key = RedisModule_CreateStringPrintf(NULL, "__source__");
    labels[2].value = RedisModule_CreateStringPrintf(NULL, "");
    return labels;
}

TS_ResultSet *ResultSet_Create() {
    TS_ResultSet *r = malloc(sizeof(TS_ResultSet));
    r->groups = RedisModule_CreateDict(NULL);
    r->labelkey = NULL;
    return r;
}

int GroupList_SetLabelValue(TS_GroupList *r, const char *label) {
    r->labelValue = strdup(label);
    return true;
}

int ResultSet_GroupbyLabel(TS_ResultSet *r, const char *label) {
    r->labelkey = strdup(label);
    return true;
}

int ResultSet_ApplyReducer(TS_ResultSet *r,
                           RangeArgs *args,
                           MultiSeriesReduceOp reducerOp,
                           bool reverse) {
    // ^ seek the smallest element of the radix tree.
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(r->groups, "^", NULL, 0);
    TS_GroupList *groupList;
    while (RedisModule_DictNextC(iter, NULL, (void **)&groupList) != NULL) {
        GroupList_ApplyReducer(groupList, r->labelkey, args, reducerOp, reverse);
    }
    RedisModule_DictIteratorStop(iter);

    return TSDB_OK;
}
void GroupList_ApplyReducer(TS_GroupList *group,
                            char *labelKey,
                            RangeArgs *args,
                            MultiSeriesReduceOp reducerOp,
                            bool reverse) {
    Label *labels = createReducedSeriesLabels(labelKey, group->labelValue, reducerOp);
    size_t serie_name_len = strlen(labelKey) + strlen(group->labelValue) + 2;
    char *serie_name = malloc(serie_name_len);
    serie_name_len = sprintf(serie_name, "%s=%s", labelKey, group->labelValue);

    // create a temp serie
    CreateCtx cCtx = {
        .labels = NULL, .labelsCount = 0, .chunkSizeBytes = Chunk_SIZE_BYTES_SECS, .options = 0
    };
    cCtx.options |= SERIES_OPT_UNCOMPRESSED;
    cCtx.isTemporary = true;

    Series *reduced = NewSeries(RedisModule_CreateString(NULL, serie_name, serie_name_len), &cCtx);

    Series *source = NULL;
    for (int i = 0; i < group->count; i++) {
        source = group->list[i];
        MultiSerieReduce(reduced, source, reducerOp, args, reverse);

        size_t keyLen = 0;
        const char *keyname = RedisModule_StringPtrLen(source->keyName, &keyLen);
        RedisModule_StringAppendBuffer(NULL, labels[2].value, keyname, keyLen);
        // check if its the last item in the group, if not append a comma
        if (i < group->count - 1) {
            RedisModule_StringAppendBuffer(NULL, labels[2].value, ",", 1);
        }
    }
    group->list[0] = reduced;
    group->count = 1;

    // replace labels
    FreeLabels(reduced->labels, reduced->labelsCount);
    reduced->labels = labels;
    reduced->labelsCount = 3;

    free(serie_name);
}

int ResultSet_AddSerie(TS_ResultSet *r, Series *serie, const char *name) {
    int result = false;

    char *labelValue = SeriesGetCStringLabelValue(serie, r->labelkey);
    if (labelValue != NULL) {
        const size_t labelLen = strlen(labelValue);
        int nokey;
        TS_GroupList *labelGroup =
            (TS_GroupList *)RedisModule_DictGetC(r->groups, (void *)labelValue, labelLen, &nokey);
        if (nokey) {
            labelGroup = GroupList_Create();
            GroupList_SetLabelValue(labelGroup, labelValue);
            RedisModule_DictSetC(r->groups, (void *)labelValue, labelLen, labelGroup);
        }
        free(labelValue);
        result = GroupList_AddSerie(labelGroup, serie, name);
    }

    return result;
}

void replyResultSet(RedisModuleCtx *ctx,
                    TS_ResultSet *r,
                    bool withlabels,
                    RedisModuleString *limitLabels[],
                    ushort limitLabelsSize,
                    RangeArgs *args,
                    bool rev) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(r->groups, "^", NULL, 0);

    RedisModule_ReplyWithArray(ctx, RedisModule_DictSize(r->groups));
    TS_GroupList *innerGroupList;
    while (RedisModule_DictNextC(iter, NULL, (void **)&innerGroupList) != NULL) {
        GroupList_ReplyResultSet(
            ctx, innerGroupList, withlabels, limitLabels, limitLabelsSize, args, rev);
    }

    RedisModule_DictIteratorStop(iter);
}

void ResultSet_Free(TS_ResultSet *r) {
    if (r == NULL)
        return;
    if (r->groups) {
        RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(r->groups, "^", NULL, 0);
        TS_GroupList *innerGroupList;
        while (RedisModule_DictNextC(iter, NULL, (void **)&innerGroupList) != NULL) {
            GroupList_Free(innerGroupList);
        }
        RedisModule_DictIteratorStop(iter);
        RedisModule_FreeDict(NULL, r->groups);
    }
    if (r->labelkey)
        free(r->labelkey);
    free(r);
}
