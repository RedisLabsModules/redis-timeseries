
#include "gears_integration.h"

#include "RedisModulesSDK/redismodule.h"
#include "consts.h"
#include "generic_chunk.h"
#include "indexer.h"
#include "query_language.h"
#include "redisgears.h"
#include "tsdb.h"

#include <assert.h>
#include "rmutil/alloc.h"

#define QueryPredicatesVersion 1
#define SeriesRecordName "SeriesRecord"

static RecordType *SeriesRecordType = NULL;
static bool GearsLoaded = false;

RecordType *GetSeriesRecordType() {
    return SeriesRecordType;
}

static void QueryPredicates_ObjectFree(void *arg) {
    QueryPredicates_Arg *predicate_list = arg;

    QueryPredicateList_Free(predicate_list->predicates);
    free(predicate_list);
}

static void *QueryPredicates_Duplicate(void *arg) {
    assert(FALSE);
}

static char *QueryPredicates_ToString(void *arg) {
    QueryPredicates_Arg *predicate_list = arg;
    char out[250];
    int index = 0;
    index += sprintf(out, "QueryPredicates: len: %lu; ", predicate_list->predicates->count);
    for (int i = 0; i < predicate_list->predicates->count; i++) {
        QueryPredicate *predicate = predicate_list->predicates->list + i;
        size_t len;
        index += sprintf(out + index,
                         "'%s=%s' ",
                         RedisModule_StringPtrLen(predicate->key, &len),
                         RedisModule_StringPtrLen(predicate->valuesList[0], &len));
    }
    return strdup(out);
}

static void BWWriteRedisString(Gears_BufferWriter *bw, const RedisModuleString *arg);

static int QueryPredicates_ArgSerialize(FlatExecutionPlan *fep,
                                        void *arg,
                                        Gears_BufferWriter *bw,
                                        char **err) {
    QueryPredicates_Arg *predicate_list = arg;
    RedisGears_BWWriteLong(bw, predicate_list->predicates->count);
    RedisGears_BWWriteLong(bw, predicate_list->withLabels);
    RedisGears_BWWriteLong(bw, predicate_list->startTimestamp);
    RedisGears_BWWriteLong(bw, predicate_list->endTimestamp);
    for (int i = 0; i < predicate_list->predicates->count; i++) {
        // encode type
        QueryPredicate *predicate = predicate_list->predicates->list + i;
        RedisGears_BWWriteLong(bw, predicate->type);

        // encode key
        BWWriteRedisString(bw, predicate->key);

        // encode values
        RedisGears_BWWriteLong(bw, predicate->valueListCount);
        for (int value_index = 0; value_index < predicate->valueListCount; value_index++) {
            BWWriteRedisString(bw, predicate->valuesList[value_index]);
        }
    }
    return REDISMODULE_OK;
}

static void BWWriteRedisString(Gears_BufferWriter *bw, const RedisModuleString *arg) {
    size_t value_len = 0;
    const char *value = RedisModule_StringPtrLen(arg, &value_len);
    RedisGears_BWWriteBuffer(bw, value, value_len + 1);
}

static RedisModuleString *BRReadRedisString(Gears_BufferReader *br) {
    const char *temp = RedisGears_BRReadString(br);
    size_t len = strlen(temp);
    return RedisModule_CreateString(NULL, temp, len);
}

static void *QueryPredicates_ArgDeserialize(FlatExecutionPlan *fep,
                                            Gears_BufferReader *br,
                                            int version,
                                            char **err) {
    QueryPredicates_Arg *predicates = malloc(sizeof(*predicates));
    predicates->predicates = malloc(sizeof(QueryPredicateList));
    predicates->predicates->count = RedisGears_BRReadLong(br);
    predicates->predicates->ref = 1;
    predicates->withLabels = RedisGears_BRReadLong(br);
    predicates->startTimestamp = RedisGears_BRReadLong(br);
    predicates->endTimestamp = RedisGears_BRReadLong(br);

    predicates->predicates->list = calloc(predicates->predicates->count, sizeof(QueryPredicate));
    for (int i = 0; i < predicates->predicates->count; i++) {
        QueryPredicate *predicate = predicates->predicates->list + i;
        // decode type
        predicate->type = RedisGears_BRReadLong(br);

        // decode key
        predicate->key = BRReadRedisString(br);

        // decode values
        predicate->valueListCount = RedisGears_BRReadLong(br);
        predicate->valuesList = calloc(predicate->valueListCount, sizeof(RedisModuleString *));

        for (int value_index = 0; value_index < predicate->valueListCount; value_index++) {
            predicate->valuesList[value_index] = BRReadRedisString(br);
        }
    }
    return predicates;
}

Record *RedisGears_RedisStringRecordCreate(RedisModuleString *str) {
    size_t len = 0;
    const char *cstr = RedisModule_StringPtrLen(str, &len);
    return RedisGears_StringRecordCreate(strndup(cstr, len), len);
}

Record *ListSeriesLabels(const Series *series) {
    Record *r = RedisGears_ListRecordCreate(series->labelsCount);
    for (int i = 0; i < series->labelsCount; i++) {
        Record *internal_list = RedisGears_ListRecordCreate(series->labelsCount);
        RedisGears_ListRecordAdd(internal_list,
                                 RedisGears_RedisStringRecordCreate(series->labels[i].key));
        RedisGears_ListRecordAdd(internal_list,
                                 RedisGears_RedisStringRecordCreate(series->labels[i].value));
        RedisGears_ListRecordAdd(r, internal_list);
    }
    return r;
}

#define MAX_VAL_LEN 24
Record *ListWithSample(u_int64_t timestamp, double value) {
    Record *r = RedisGears_ListRecordCreate(2);
    RedisGears_ListRecordAdd(r, RedisGears_LongRecordCreate(timestamp));
    char buf[MAX_VAL_LEN];
    snprintf(buf, MAX_VAL_LEN, "%.15g", value);
    RedisGears_ListRecordAdd(r, RedisGears_StringRecordCreate(strdup(buf), strlen(buf)));
    return r;
}

Record *ListWithSeriesLastDatapoint(const Series *series) {
    if (SeriesGetNumSamples(series) == 0) {
        return RedisGears_ListRecordCreate(0);
    } else {
        return ListWithSample(series->lastTimestamp, VALUE_DOUBLE(&series->lastValue));
    }
}

Record *ShardSeriesMapper(ExecutionCtx *rctx, Record *data, void *arg) {
    RedisModuleCtx *ctx = RedisGears_GetRedisModuleCtx(rctx);
    QueryPredicates_Arg *predicates = arg;

    RedisModuleDict *result =
        QueryIndex(ctx, predicates->predicates->list, predicates->predicates->count);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;

    Series *series;
    Record *series_list = RedisGears_ListRecordCreate(0);
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        RedisModuleString *keyName = RedisModule_CreateString(ctx, currentKey, currentKeyLen);
        const int status = SilentGetSeries(ctx, keyName, &key, &series, REDISMODULE_READ);
        RedisModule_FreeString(ctx, keyName);

        if (!status) {
            RedisModule_Log(ctx,
                            "warning",
                            "couldn't open key or key is not a Timeseries. key=%.*s",
                            currentKeyLen,
                            currentKey);
            continue;
        }
        RedisGears_ListRecordAdd(
            series_list,
            SeriesRecord_New(series, predicates->startTimestamp, predicates->endTimestamp));
        RedisModule_CloseKey(key);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_FreeDict(ctx, result);
    RedisGears_FreeRecord(data);

    return series_list;
}

Record *ShardMgetMapper(ExecutionCtx *rctx, Record *data, void *arg) {
    RedisModuleCtx *ctx = RedisGears_GetRedisModuleCtx(rctx);
    QueryPredicates_Arg *predicates = arg;

    RedisModuleDict *result =
        QueryIndex(ctx, predicates->predicates->list, predicates->predicates->count);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;

    Series *series;
    Record *series_list = RedisGears_ListRecordCreate(0);
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        RedisModuleString *keyName = RedisModule_CreateString(ctx, currentKey, currentKeyLen);
        const int status = SilentGetSeries(ctx, keyName, &key, &series, REDISMODULE_READ);
        RedisModule_FreeString(ctx, keyName);

        if (!status) {
            RedisModule_Log(ctx,
                            "warning",
                            "couldn't open key or key is not a Timeseries. key=%.*s",
                            currentKeyLen,
                            currentKey);
            continue;
        }

        Record *key_record = RedisGears_ListRecordCreate(3);
        RedisGears_ListRecordAdd(
            key_record,
            RedisGears_StringRecordCreate(strndup(currentKey, currentKeyLen), currentKeyLen));
        if (predicates->withLabels) {
            RedisGears_ListRecordAdd(key_record, ListSeriesLabels(series));
        } else {
            RedisGears_ListRecordAdd(key_record, RedisGears_ListRecordCreate(0));
        }
        RedisGears_ListRecordAdd(key_record, ListWithSeriesLastDatapoint(series));
        RedisModule_CloseKey(key);

        RedisGears_ListRecordAdd(series_list, key_record);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_FreeDict(ctx, result);
    RedisGears_FreeRecord(data);

    return series_list;
}

Record *ShardQueryindexMapper(ExecutionCtx *rctx, Record *data, void *arg) {
    RedisModuleCtx *ctx = RedisGears_GetRedisModuleCtx(rctx);
    QueryPredicates_Arg *predicates = arg;

    RedisModuleDict *result =
        QueryIndex(ctx, predicates->predicates->list, predicates->predicates->count);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;

    Record *series_list = RedisGears_ListRecordCreate(0);
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisGears_ListRecordAdd(
            series_list,
            RedisGears_StringRecordCreate(strndup(currentKey, currentKeyLen), currentKeyLen));
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_FreeDict(ctx, result);
    RedisGears_FreeRecord(data);

    return series_list;
}

int register_rg(RedisModuleCtx *ctx) {
    if (RedisGears_InitAsRedisModule(ctx, "timeseries", REDISMODULE_TYPE_METHOD_VERSION) !=
        REDISMODULE_OK) {
        RedisModule_Log(ctx, "warning", "Failed initialize RedisGears API");
        return REDISMODULE_ERR;
    }

    ArgType *QueryPredicatesType = RedisGears_CreateType("QueryPredicatesType",
                                                         QueryPredicatesVersion,
                                                         QueryPredicates_ObjectFree,
                                                         QueryPredicates_Duplicate,
                                                         QueryPredicates_ArgSerialize,
                                                         QueryPredicates_ArgDeserialize,
                                                         QueryPredicates_ToString);

    SeriesRecordType = RedisGears_RecordTypeCreate(SeriesRecordName,
                                                   sizeof(SeriesRecord),
                                                   SeriesRecord_SendReply,
                                                   (RecordSerialize)SeriesRecord_Serialize,
                                                   (RecordDeserialize)SeriesRecord_Deserialize,
                                                   (RecordFree)SeriesRecord_ObjectFree);
    if (RedisGears_RegisterMap("ShardSeriesMapper", ShardSeriesMapper, QueryPredicatesType) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisGears_RegisterMap("ShardMgetMapper", ShardMgetMapper, QueryPredicatesType) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisGears_RegisterMap("ShardQueryindexMapper",
                               ShardQueryindexMapper,
                               QueryPredicatesType) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    GearsLoaded = true;
    return REDISMODULE_OK;
}

bool IsGearsLoaded() {
    return GearsLoaded;
}

Record *SeriesRecord_New(Series *series, timestamp_t startTimestamp, timestamp_t endTimestamp) {
    SeriesRecord *out = (SeriesRecord *)RedisGears_RecordCreate(SeriesRecordType);
    out->keyName = RedisModule_CreateStringFromString(NULL, series->keyName);
    if (series->options & SERIES_OPT_UNCOMPRESSED) {
        out->chunkType = CHUNK_REGULAR;
    } else {
        out->chunkType = CHUNK_COMPRESSED;
    }
    out->funcs = series->funcs;
    out->labelsCount = series->labelsCount;
    out->labels = calloc(series->labelsCount, sizeof(Label));
    for (int i = 0; i < series->labelsCount; i++) {
        out->labels[i].key = RedisModule_CreateStringFromString(NULL, series->labels[i].key);
        out->labels[i].value = RedisModule_CreateStringFromString(NULL, series->labels[i].value);
    }

    // clone chunks
    out->chunks = calloc(RedisModule_DictSize(series->chunks), sizeof(Chunk_t *));
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    Chunk_t *chunk = NULL;
    int index = 0;
    while (RedisModule_DictNextC(iter, NULL, &chunk)) {
        if (series->funcs->GetLastTimestamp(chunk) > startTimestamp) {
            if (series->funcs->GetFirstTimestamp(chunk) > endTimestamp) {
                break;
            }

            out->chunks[index] = out->funcs->CloneChunk(chunk);
            index++;
        }
    }
    out->chunkCount = index;
    RedisModule_DictIteratorStop(iter);
    return &out->base;
}

void SeriesRecord_ObjectFree(void *record) {
    SeriesRecord *series = record;
    for (int i = 0; i < series->labelsCount; i++) {
        RedisModule_FreeString(NULL, series->labels[i].key);
        RedisModule_FreeString(NULL, series->labels[i].value);
    }
    free(series->labels);

    for (int i = 0; i < series->chunkCount; i++) {
        series->funcs->FreeChunk(series->chunks[i]);
    }

    free(series->chunks);
    RedisModule_FreeString(NULL, series->keyName);
}

int SeriesRecord_Serialize(ExecutionCtx *ctx, Gears_BufferWriter *bw, Record *base) {
    SeriesRecord *series = (SeriesRecord *)base;
    RedisGears_BWWriteLong(bw, series->chunkType);
    BWWriteRedisString(bw, series->keyName);
    RedisGears_BWWriteLong(bw, series->labelsCount);
    for (int i = 0; i < series->labelsCount; i++) {
        BWWriteRedisString(bw, series->labels[i].key);
        BWWriteRedisString(bw, series->labels[i].value);
    }

    RedisGears_BWWriteLong(bw, series->chunkCount);
    for (int i = 0; i < series->chunkCount; i++) {
        series->funcs->GearsSerialize(series->chunks[i], bw);
    }
    return REDISMODULE_OK;
}

Record *SeriesRecord_Deserialize(ExecutionCtx *ctx, Gears_BufferReader *br) {
    SeriesRecord *series = (SeriesRecord *)RedisGears_RecordCreate(SeriesRecordType);
    series->chunkType = RedisGears_BRReadLong(br);
    series->funcs = GetChunkClass(series->chunkType);
    series->keyName = BRReadRedisString(br);
    series->labelsCount = RedisGears_BRReadLong(br);
    series->labels = calloc(series->labelsCount, sizeof(Label));
    for (int i = 0; i < series->labelsCount; i++) {
        series->labels[i].key = BRReadRedisString(br);
        series->labels[i].value = BRReadRedisString(br);
    }

    series->chunkCount = RedisGears_BRReadLong(br);
    series->chunks = calloc(series->chunkCount, sizeof(Chunk_t *));
    for (int i = 0; i < series->chunkCount; i++) {
        series->funcs->GearsDeserialize(&series->chunks[i], br);
    }
    return &series->base;
}

int SeriesRecord_SendReply(Record *record, RedisModuleCtx *rctx) {
    SeriesRecord *series = (SeriesRecord *)record;
    RedisModule_ReplyWithArray(rctx, 3);
    RedisModule_ReplyWithString(rctx, series->keyName);
    RedisModule_ReplyWithLongLong(rctx, series->chunkCount);
    RedisModule_ReplyWithLongLong(rctx, series->labelsCount);
    return REDISMODULE_OK;
}

Series *SeriesRecord_IntoSeries(SeriesRecord *record) {
    CreateCtx createArgs = { 0 };
    createArgs.isTemporary = true;
    createArgs.skipChunkCreation = true;
    Series *s = NewSeries(RedisModule_CreateStringFromString(NULL, record->keyName), &createArgs);
    s->labelsCount = record->labelsCount;
    s->labels = calloc(s->labelsCount, sizeof(Label));
    for (int i = 0; i < s->labelsCount; i++) {
        s->labels[i].key = RedisModule_CreateStringFromString(NULL, record->labels[i].key);
        s->labels[i].value = RedisModule_CreateStringFromString(NULL, record->labels[i].value);
    }
    s->funcs = record->funcs;

    for (int chunk_index = 0; chunk_index < record->chunkCount; chunk_index++) {
        dictOperator(s->chunks,
                     s->funcs->CloneChunk(record->chunks[chunk_index]),
                     record->funcs->GetFirstTimestamp(record->chunks[chunk_index]),
                     DICT_OP_SET);
    }
    return s;
}
