/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#define REDISMODULE_MAIN

#include "module.h"

#include "RedisModulesSDK/redismodule.h"
#include "common.h"
#include "compaction.h"
#include "config.h"
#include "fast_double_parser_c/fast_double_parser_c.h"
#include "gears_commands.h"
#include "gears_integration.h"
#include "indexer.h"
#include "query_language.h"
#include "rdb.h"
#include "redisgears.h"
#include "reply.h"
#include "resultset.h"
#include "tsdb.h"
#include "version.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "rmutil/alloc.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"

#ifndef REDISTIMESERIES_GIT_SHA
#define REDISTIMESERIES_GIT_SHA "unknown"
#endif

RedisModuleType *SeriesType;

int TSDB_info(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2 || argc > 3) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    int status = GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ);
    if (!status) {
        return REDISMODULE_ERR;
    }

    int is_debug = RMUtil_ArgExists("DEBUG", argv, argc, 1);
    if (is_debug) {
        RedisModule_ReplyWithArray(ctx, 14 * 2);
    } else {
        RedisModule_ReplyWithArray(ctx, 13 * 2);
    }

    long long skippedSamples;
    long long firstTimestamp = getFirstValidTimestamp(series, &skippedSamples);

    RedisModule_ReplyWithSimpleString(ctx, "type");
    RedisModule_ReplyWithSimpleString(ctx, SeriesIsBlob(series) ? "blob" : "numeric");
    RedisModule_ReplyWithSimpleString(ctx, "totalSamples");
    RedisModule_ReplyWithLongLong(ctx, SeriesGetNumSamples(series) - skippedSamples);
    RedisModule_ReplyWithSimpleString(ctx, "memoryUsage");
    RedisModule_ReplyWithLongLong(ctx, SeriesMemUsage(series));
    RedisModule_ReplyWithSimpleString(ctx, "firstTimestamp");
    RedisModule_ReplyWithLongLong(ctx, firstTimestamp);
    RedisModule_ReplyWithSimpleString(ctx, "lastTimestamp");
    RedisModule_ReplyWithLongLong(ctx, series->lastTimestamp);
    RedisModule_ReplyWithSimpleString(ctx, "retentionTime");
    RedisModule_ReplyWithLongLong(ctx, series->retentionTime);
    RedisModule_ReplyWithSimpleString(ctx, "chunkCount");
    RedisModule_ReplyWithLongLong(ctx, RedisModule_DictSize(series->chunks));
    RedisModule_ReplyWithSimpleString(ctx, "chunkSize");
    RedisModule_ReplyWithLongLong(ctx, series->chunkSizeBytes);
    RedisModule_ReplyWithSimpleString(ctx, "chunkType");
    if (series->options & SERIES_OPT_UNCOMPRESSED) {
        RedisModule_ReplyWithSimpleString(ctx, "uncompressed");
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "compressed");
    };
    RedisModule_ReplyWithSimpleString(ctx, "duplicatePolicy");
    if (series->duplicatePolicy != DP_NONE) {
        RedisModule_ReplyWithSimpleString(ctx, DuplicatePolicyToString(series->duplicatePolicy));
    } else {
        RedisModule_ReplyWithNull(ctx);
    }

    RedisModule_ReplyWithSimpleString(ctx, "labels");
    ReplyWithSeriesLabels(ctx, series);

    RedisModule_ReplyWithSimpleString(ctx, "sourceKey");
    if (series->srcKey == NULL) {
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithString(ctx, series->srcKey);
    }

    RedisModule_ReplyWithSimpleString(ctx, "rules");

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    CompactionRule *rule = series->rules;
    int ruleCount = 0;
    while (rule != NULL) {
        RedisModule_ReplyWithArray(ctx, 3);
        RedisModule_ReplyWithString(ctx, rule->destKey);
        RedisModule_ReplyWithLongLong(ctx, rule->timeBucket);
        RedisModule_ReplyWithSimpleString(ctx, AggTypeEnumToString(rule->aggType));

        rule = rule->nextRule;
        ruleCount++;
    }
    RedisModule_ReplySetArrayLength(ctx, ruleCount);

    if (is_debug) {
        RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, ">", "", 0);
        Chunk_t *chunk = NULL;
        int chunkCount = 0;
        RedisModule_ReplyWithSimpleString(ctx, "Chunks");
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        while (RedisModule_DictNextC(iter, NULL, (void *)&chunk)) {
            size_t chunkSize = series->funcs->GetChunkSize(chunk, FALSE);
            RedisModule_ReplyWithArray(ctx, 5 * 2);
            RedisModule_ReplyWithSimpleString(ctx, "startTimestamp");
            RedisModule_ReplyWithLongLong(ctx, series->funcs->GetFirstTimestamp(chunk));
            RedisModule_ReplyWithSimpleString(ctx, "endTimestamp");
            RedisModule_ReplyWithLongLong(ctx, series->funcs->GetLastTimestamp(chunk));
            RedisModule_ReplyWithSimpleString(ctx, "samples");
            u_int64_t numOfSamples = series->funcs->GetNumOfSample(chunk);
            RedisModule_ReplyWithLongLong(ctx, numOfSamples);
            RedisModule_ReplyWithSimpleString(ctx, "size");
            RedisModule_ReplyWithLongLong(ctx, chunkSize);
            RedisModule_ReplyWithSimpleString(ctx, "bytesPerSample");
            RedisModule_ReplyWithDouble(ctx, (float)chunkSize / numOfSamples);
            chunkCount++;
        }
        RedisModule_DictIteratorStop(iter);
        RedisModule_ReplySetArrayLength(ctx, chunkCount);
    }
    RedisModule_CloseKey(key);

    return REDISMODULE_OK;
}

void _TSDB_queryindex_impl(RedisModuleCtx *ctx, QueryPredicateList *queries) {
    RedisModuleDict *result = QueryIndex(ctx, queries->list, queries->count);

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModule_ReplyWithStringBuffer(ctx, currentKey, currentKeyLen);
        replylen++;
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_ReplySetArrayLength(ctx, replylen);
}

int TSDB_queryindex(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    int query_count = argc - 1;

    int response = 0;
    QueryPredicateList *queries = parseLabelListFromArgs(ctx, argv, 1, query_count, &response);
    if (response == TSDB_ERROR) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, EQ) + CountPredicateType(queries, LIST_MATCH) == 0) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    if (IsGearsLoaded()) {
        TSDB_queryindex_RG(ctx, queries);
        QueryPredicateList_Free(queries);
    } else {
        _TSDB_queryindex_impl(ctx, queries);
        QueryPredicateList_Free(queries);
    }

    return REDISMODULE_OK;
}

// multi-series groupby logic
static int replyGroupedMultiRange(RedisModuleCtx *ctx,
                                  TS_ResultSet *resultset,
                                  RedisModuleDict *result,
                                  const MRangeArgs *args) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey = NULL;
    size_t currentKeyLen;
    Series *series = NULL;

    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const int status = SilentGetSeries(ctx,
                                           RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                                           &key,
                                           &series,
                                           REDISMODULE_READ);
        if (!status) {
            RedisModule_Log(
                ctx, "warning", "couldn't open key or key is not a Timeseries. key=%s", currentKey);
            // The iterator may have been invalidated, stop and restart from after the current
            // key.
            RedisModule_DictIteratorStop(iter);
            iter = RedisModule_DictIteratorStartC(result, ">", currentKey, currentKeyLen);
            continue;
        }
        ResultSet_AddSerie(resultset, series, RedisModule_StringPtrLen(series->keyName, NULL));
        RedisModule_CloseKey(key);
    }
    RedisModule_DictIteratorStop(iter);

    // todo: this is duplicated in resultset.c
    // Apply the reducer
    ResultSet_ApplyReducer(resultset, &args->rangeArgs, args->gropuByReducerOp, args->reverse);

    // Do not apply the aggregation on the resultset, do apply max results on the final result
    RangeArgs minimizedArgs = args->rangeArgs;
    minimizedArgs.aggregationArgs.aggregationClass = NULL;
    minimizedArgs.aggregationArgs.timeDelta = 0;
    minimizedArgs.filterByTSArgs.hasValue = false;
    minimizedArgs.filterByValueArgs.hasValue = false;

    replyResultSet(ctx, resultset, args->withLabels, &minimizedArgs, args->reverse);

    ResultSet_Free(resultset);
    return REDISMODULE_OK;
}

// Previous multirange reply logic ( unchanged )
static int replyUngroupedMultiRange(RedisModuleCtx *ctx,
                                    RedisModuleDict *result,
                                    const MRangeArgs *args) {
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const int status = SilentGetSeries(ctx,
                                           RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                                           &key,
                                           &series,
                                           REDISMODULE_READ);

        if (!status) {
            RedisModule_Log(ctx,
                            "couldn't open key or key is not a Timeseries. key=%.*s",
                            currentKeyLen,
                            currentKey);
            // The iterator may have been invalidated, stop and restart from after the current key.
            RedisModule_DictIteratorStop(iter);
            iter = RedisModule_DictIteratorStartC(result, ">", currentKey, currentKeyLen);
            continue;
        }
        ReplySeriesArrayPos(ctx, series, args->withLabels, &args->rangeArgs, args->reverse);
        replylen++;
        RedisModule_CloseKey(key);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_ReplySetArrayLength(ctx, replylen);

    return REDISMODULE_OK;
}

int TSDB_generic_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
    RedisModule_AutoMemory(ctx);

    MRangeArgs args;
    if (parseMRangeCommand(ctx, argv, argc, &args) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    args.reverse = rev;

    RedisModuleDict *resultSeries =
        QueryIndex(ctx, args.queryPredicates->list, args.queryPredicates->count);

    int result = REDISMODULE_OK;
    if (args.groupByLabel) {
        TS_ResultSet *resultset = ResultSet_Create();
        ResultSet_GroupbyLabel(resultset, args.groupByLabel);

        result = replyGroupedMultiRange(ctx, resultset, resultSeries, &args);
    } else {
        result = replyUngroupedMultiRange(ctx, resultSeries, &args);
    }

    MRangeArgs_Free(&args);
    return result;
}

int TSDB_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsGearsLoaded()) {
        return TSDB_mrange_RG(ctx, argv, argc, false);
    }

    return TSDB_generic_mrange(ctx, argv, argc, false);
}

int TSDB_mrevrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsGearsLoaded()) {
        return TSDB_mrange_RG(ctx, argv, argc, true);
    }
    return TSDB_generic_mrange(ctx, argv, argc, true);
}

int TSDB_generic_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
    TS_AGG_TYPES_T agg_type = TS_AGG_NONE;

    RedisModule_AutoMemory(ctx);

    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    const int status = GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ);
    if (!status) {
        return REDISMODULE_ERR;
    }

    RangeArgs rangeArgs = { 0 };
    if (parseRangeArguments(ctx, 2, argv, argc, series->lastTimestamp, &rangeArgs) !=
        REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    agg_type = rangeArgs.aggregationArgs.type;
    if (SeriesIsBlob(series) && !IsCompactionBlobFriendly(agg_type)) {
        RTS_ReplyGeneralError(ctx, "Aggregation type is not allowed with blob series");
        return REDISMODULE_ERR;
    }

    ReplySeriesRange(ctx, series, &rangeArgs, rev);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int TSDB_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, false);
}

int TSDB_revrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, true);
}

static void handleCompaction(RedisModuleCtx *ctx,
                             Series *series,
                             CompactionRule *rule,
                             api_timestamp_t timestamp,
                             SampleValue sValue) {
    timestamp_t currentTimestamp = CalcWindowStart(timestamp, rule->timeBucket);

    if (rule->startCurrentTimeBucket == -1LL) {
        // first sample, lets init the startCurrentTimeBucket
        rule->startCurrentTimeBucket = currentTimestamp;
    }

    if (currentTimestamp > rule->startCurrentTimeBucket) {
        RedisModuleKey *key =
            RedisModule_OpenKey(ctx, rule->destKey, REDISMODULE_READ | REDISMODULE_WRITE);
        if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
            // key doesn't exist anymore and we don't do anything
            return;
        }
        Series *destSeries = RedisModule_ModuleTypeGetValue(key);

        SampleValue sAggVal;

        if (aggClassIsBlob(rule->aggClass))
            VALUE_BLOB(&sAggVal) = NewBlob(EMPTY_BLOB, EMPTY_BLOB_SIZE);

        if (rule->aggClass->finalize(rule->aggContext, &VALUE_DOUBLE(&sAggVal)) == TSDB_OK) {
            SeriesAddSample(destSeries, rule->startCurrentTimeBucket, sAggVal);
            RedisModule_NotifyKeyspaceEvent(
                ctx, REDISMODULE_NOTIFY_MODULE, "ts.add:dest", rule->destKey);
        }

        if (aggClassIsBlob(rule->aggClass))
            FreeBlob(VALUE_BLOB(&sAggVal));

        rule->aggClass->resetContext(rule->aggContext);
        rule->startCurrentTimeBucket = currentTimestamp;
        RedisModule_CloseKey(key);
    }
    rule->aggClass->appendValue(rule->aggContext, VALUE_DOUBLE(&sValue));
}

static int internalAdd(RedisModuleCtx *ctx,
                       Series *series,
                       api_timestamp_t timestamp,
                       SampleValue sValue,
                       DuplicatePolicy dp_override) {
    timestamp_t lastTS = series->lastTimestamp;
    uint64_t retention = series->retentionTime;
    // ensure inside retention period.
    if (retention && timestamp < lastTS && retention < lastTS - timestamp) {
        RTS_ReplyGeneralError(ctx, "TSDB: Timestamp is older than retention");
        return REDISMODULE_ERR;
    }

    if (timestamp <= series->lastTimestamp && series->totalSamples != 0) {
        if (SeriesUpsertSample(series, timestamp, sValue, dp_override) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx,
                                  "TSDB: Error at upsert, update is not supported in BLOCK mode");
            return REDISMODULE_ERR;
        }
    } else {
        if (SeriesAddSample(series, timestamp, sValue) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Error at add");
            return REDISMODULE_ERR;
        }
        // handle compaction rules
        CompactionRule *rule = series->rules;
        while (rule != NULL) {
            handleCompaction(ctx, series, rule, timestamp, sValue);
            rule = rule->nextRule;
        }
    }
    RedisModule_ReplyWithLongLong(ctx, timestamp);
    return REDISMODULE_OK;
}

static int parseValue(RedisModuleCtx *ctx,
                      bool isBlob,
                      const RedisModuleString *valueStr,
                      SampleValue *sampleValue) {
    if (isBlob) {
        size_t len;
        const char *data = RedisModule_StringPtrLen(valueStr, &len);
        VALUE_BLOB(sampleValue) = NewBlob(data, len);
        return 0;
    }

    const char *valueCStr = RedisModule_StringPtrLen(valueStr, NULL);
    if ((fast_double_parser_c_parse_number(valueCStr, &VALUE_DOUBLE(sampleValue)) == NULL)) {
        return -1;
    }
    return 0;
}

static inline int add(RedisModuleCtx *ctx,
                      RedisModuleString *keyName,
                      RedisModuleString *timestampStr,
                      RedisModuleString *valueStr,
                      RedisModuleString **argv,
                      int argc) {
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);
    SampleValue value;

    long long timestampValue;
    if ((RedisModule_StringToLongLong(timestampStr, &timestampValue) != REDISMODULE_OK)) {
        // if timestamp is "*", take current time (automatic timestamp)
        if (RMUtil_StringEqualsC(timestampStr, "*"))
            timestampValue = RedisModule_Milliseconds();
        else
            return RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp");
    }

    if (timestampValue < 0) {
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp, must be positive number");
    }
    api_timestamp_t timestamp = (u_int64_t)timestampValue;

    Series *series = NULL;
    DuplicatePolicy dp = DP_NONE;
    CreateCtx cCtx = { 0 };

    if (argv != NULL && RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        // the key doesn't exist, lets check we have enough information to create one

        if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
            RedisModule_CloseKey(key);
            return RTS_ReplyGeneralError(ctx, "TSDB: unable to parse create arguments");
        }

    } else if (RedisModule_ModuleTypeGetType(key) != SeriesType) {
        return RTS_ReplyGeneralError(ctx, "TSDB: the key is not a TSDB key");
    } else {
        series = RedisModule_ModuleTypeGetValue(key);
        //  overwride key and database configuration for DUPLICATE_POLICY
        if (argv != NULL &&
            ParseDuplicatePolicy(ctx, argv, argc, TS_ADD_DUPLICATE_POLICY_ARG, &dp) != TSDB_OK) {
            return REDISMODULE_ERR;
        }
    }

    bool isBlob = false;
    if ((cCtx.options & SERIES_OPT_BLOB) == SERIES_OPT_BLOB || (series && SeriesIsBlob(series)))
        isBlob = true;

    /* when the given object is a blob, it is allocated by this call */
    if (parseValue(ctx, isBlob, valueStr, &value) != 0) {
        RedisModule_CloseKey(key);
        return RTS_ReplyGeneralError(ctx, "TSDB: unable to parse value");
    }

    if (!series) {
        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    }

    int rv = internalAdd(ctx, series, timestamp, value, dp);

    if (isBlob) {
        FreeBlob(VALUE_BLOB(&value));
    }

    RedisModule_CloseKey(key);

    return rv;
}

int TSDB_madd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 4 || (argc - 1) % 3 != 0) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_ReplyWithArray(ctx, (argc - 1) / 3);
    for (int i = 1; i < argc; i += 3) {
        RedisModuleString *keyName = argv[i];
        RedisModuleString *timestampStr = argv[i + 1];
        RedisModuleString *valueStr = argv[i + 2];
        add(ctx, keyName, timestampStr, valueStr, NULL, -1);
    }
    RedisModule_ReplicateVerbatim(ctx);

    for (int i = 1; i < argc; i += 3) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.add", argv[i]);
    }

    return REDISMODULE_OK;
}

int TSDB_add(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *keyName = argv[1];
    RedisModuleString *timestampStr = argv[2];
    RedisModuleString *valueStr = argv[3];

    int result = add(ctx, keyName, timestampStr, valueStr, argv, argc);
    RedisModule_ReplicateVerbatim(ctx);

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.add", keyName);

    return result;
}

int CreateTsKey(RedisModuleCtx *ctx,
                RedisModuleString *keyName,
                CreateCtx *cCtx,
                Series **series,
                RedisModuleKey **key) {
    if (*key == NULL) {
        *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);
    }

    RedisModule_RetainString(ctx, keyName);
    *series = NewSeries(keyName, cCtx);
    if (RedisModule_ModuleTypeSetValue(*key, SeriesType, *series) == REDISMODULE_ERR) {
        return TSDB_ERROR;
    }

    IndexMetric(ctx, keyName, (*series)->labels, (*series)->labelsCount);

    return TSDB_OK;
}

int TSDB_create(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleString *keyName = argv[1];
    CreateCtx cCtx = { 0 };
    if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);

    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(key);
        return RTS_ReplyGeneralError(ctx, "TSDB: key already exists");
    }

    CreateTsKey(ctx, keyName, &cCtx, &series, &key);
    RedisModule_CloseKey(key);

    RedisModule_Log(ctx, "verbose", "created new series");
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.create", keyName);

    return REDISMODULE_OK;
}

int TSDB_alter(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    RedisModuleString *keyName = argv[1];
    CreateCtx cCtx = { 0 };
    if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    const int status = GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!status) {
        return REDISMODULE_ERR;
    }
    if (RMUtil_ArgIndex("RETENTION", argv, argc) > 0) {
        series->retentionTime = cCtx.retentionTime;
    }

    if (RMUtil_ArgIndex("CHUNK_SIZE", argv, argc) > 0) {
        series->chunkSizeBytes = cCtx.chunkSizeBytes;
    }

    if (RMUtil_ArgIndex("DUPLICATE_POLICY", argv, argc) > 0) {
        series->duplicatePolicy = cCtx.duplicatePolicy;
    }

    if (RMUtil_ArgIndex("LABELS", argv, argc) > 0) {
        RemoveIndexedMetric(ctx, keyName, series->labels, series->labelsCount);
        // free current labels
        FreeLabels(series->labels, series->labelsCount);

        // set new newLabels
        series->labels = cCtx.labels;
        series->labelsCount = cCtx.labelsCount;
        IndexMetric(ctx, keyName, series->labels, series->labelsCount);
    }
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "ts.alter", keyName);

    return REDISMODULE_OK;
}

/*
TS.DELETERULE SOURCE_KEY DEST_KEY
 */
int TSDB_deleteRule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *srcKeyName = argv[1];

    // First try to remove the rule from the source key
    Series *srcSeries;
    RedisModuleKey *srcKey;
    const int statusS =
        GetSeries(ctx, srcKeyName, &srcKey, &srcSeries, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!statusS) {
        return REDISMODULE_ERR;
    }

    RedisModuleString *destKeyName = argv[2];
    if (!SeriesDeleteRule(srcSeries, destKeyName)) {
        return RTS_ReplyGeneralError(ctx, "TSDB: compaction rule does not exist");
    }

    // If succeed to remove the rule from the source key remove from the destination too
    Series *destSeries;
    RedisModuleKey *destKey;
    const int statusD =
        GetSeries(ctx, destKeyName, &destKey, &destSeries, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!statusD) {
        return REDISMODULE_ERR;
    }
    SeriesDeleteSrcRule(destSeries, srcKeyName);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(srcKey);
    RedisModule_CloseKey(destKey);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.deleterule:src", srcKeyName);
    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.deleterule:dest", destKeyName);

    return REDISMODULE_OK;
}

/*
TS.CREATERULE sourceKey destKey AGGREGATION aggregationType timeBucket
*/
int TSDB_createRule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 6) {
        return RedisModule_WrongArity(ctx);
    }

    // Validate aggregation arguments
    api_timestamp_t timeBucket;
    int aggType;
    const int result = _parseAggregationArgs(ctx, argv, argc, &timeBucket, &aggType);
    if (result == TSDB_NOTEXISTS) {
        return RedisModule_WrongArity(ctx);
    }
    if (result == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    RedisModuleString *srcKeyName = argv[1];
    RedisModuleString *destKeyName = argv[2];
    if (!RedisModule_StringCompare(srcKeyName, destKeyName)) {
        return RTS_ReplyGeneralError(
            ctx, "TSDB: the source key and destination key should be different");
    }

    // First we verify the source is not a destination
    Series *srcSeries;
    RedisModuleKey *srcKey;
    const int statusS =
        GetSeries(ctx, srcKeyName, &srcKey, &srcSeries, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!statusS) {
        return REDISMODULE_ERR;
    }
    if (srcSeries->srcKey) {
        return RTS_ReplyGeneralError(ctx, "TSDB: the source key already has a source rule");
    }

    // Second verify the destination doesn't have other rule
    Series *destSeries;
    RedisModuleKey *destKey;
    const int statusD =
        GetSeries(ctx, destKeyName, &destKey, &destSeries, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!statusD) {
        return REDISMODULE_ERR;
    }

    if (SeriesIsBlob(destSeries) && (aggType == TS_AGG_BLOB_COUNT || aggType == TS_AGG_COUNT))
        return RTS_ReplyGeneralError(
            ctx,
            "TSDB: the destination key is of binary type and cannot hold an aggregation count");

    srcKeyName = RedisModule_CreateStringFromString(ctx, srcKeyName);
    if (!SeriesSetSrcRule(destSeries, srcKeyName)) {
        return RTS_ReplyGeneralError(ctx, "TSDB: the destination key already has a rule");
    }
    RedisModule_RetainString(ctx, srcKeyName);

    // Last add the rule to source
    destKeyName = RedisModule_CreateStringFromString(ctx, destKeyName);
    if (SeriesAddRule(srcSeries, destKeyName, aggType, timeBucket) == NULL) {
        RedisModule_ReplyWithSimpleString(ctx, "TSDB: ERROR creating rule");
        return REDISMODULE_ERR;
    }
    RedisModule_RetainString(ctx, destKeyName);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);

    RedisModule_CloseKey(srcKey);
    RedisModule_CloseKey(destKey);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.createrule:src", srcKeyName);
    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_MODULE, "ts.createrule:dest", destKeyName);

    return REDISMODULE_OK;
}

/*
TS.INCRBY ts_key NUMBER [TIMESTAMP timestamp]
*/
int TSDB_incrby(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *keyName = argv[1];
    Series *series;

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        // the key doesn't exist, lets check we have enough information to create one
        CreateCtx cCtx = { 0 };
        if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }

        if ((cCtx.options & SERIES_OPT_BLOB) == SERIES_OPT_BLOB)
            return RTS_ReplyGeneralError(ctx,
                                         "TSDB: Creating blob is forbidden with this function");

        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    } else if (RedisModule_ModuleTypeGetType(key) != SeriesType) {
        return RTS_ReplyGeneralError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    series = RedisModule_ModuleTypeGetValue(key);
    if (SeriesIsBlob(series))
        return RTS_ReplyGeneralError(ctx, "TSDB: Blobs are forbidden with this function");

    double incrby = 0;
    if (RMUtil_ParseArgs(argv, argc, 2, "d", &incrby) != REDISMODULE_OK) {
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid increase/decrease value");
    }

    long long currentUpdatedTime = -1;
    int timestampLoc = RMUtil_ArgIndex("TIMESTAMP", argv, argc);
    if (timestampLoc == -1 || RMUtil_StringEqualsC(argv[timestampLoc + 1], "*")) {
        currentUpdatedTime = RedisModule_Milliseconds();
    } else if (RedisModule_StringToLongLong(argv[timestampLoc + 1],
                                            (long long *)&currentUpdatedTime) != REDISMODULE_OK) {
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp");
    }

    if (currentUpdatedTime < series->lastTimestamp && series->lastTimestamp != 0) {
        return RedisModule_ReplyWithError(
            ctx, "TSDB: for incrby/decrby, timestamp should be newer than the lastest one");
    }

    double result = VALUE_DOUBLE(&series->lastValue);
    RMUtil_StringToLower(argv[0]);
    bool isIncr = RMUtil_StringEqualsC(argv[0], "ts.incrby");
    if (isIncr) {
        result += incrby;
    } else {
        result -= incrby;
    }
    SampleValue sValue = { .d.value = result };

    int rv = internalAdd(ctx, series, currentUpdatedTime, sValue, DP_LAST);
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);

    RedisModule_NotifyKeyspaceEvent(
        ctx, REDISMODULE_NOTIFY_GENERIC, isIncr ? "ts.incrby" : "ts.decrby", argv[1]);

    return rv;
}

int TSDB_get(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    const int status = GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ);
    if (!status) {
        return REDISMODULE_ERR;
    }

    ReplyWithSeriesLastDatapoint(ctx, series);
    RedisModule_CloseKey(key);

    return REDISMODULE_OK;
}

int TSDB_mget(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (IsGearsLoaded()) {
        return TSDB_mget_RG(ctx, argv, argc);
    }

    RedisModule_AutoMemory(ctx);

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    int filter_location = RMUtil_ArgIndex("FILTER", argv, argc);
    if (filter_location == -1) {
        return RedisModule_WrongArity(ctx);
    }
    size_t query_count = argc - 1 - filter_location;
    const int withlabels_location = RMUtil_ArgIndex("WITHLABELS", argv, argc);
    int response = 0;
    QueryPredicateList *queries =
        parseLabelListFromArgs(ctx, argv, filter_location + 1, query_count, &response);
    if (response == TSDB_ERROR) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, EQ) + CountPredicateType(queries, LIST_MATCH) == 0) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    RedisModuleDict *result = QueryIndex(ctx, queries->list, queries->count);
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const int status = SilentGetSeries(ctx,
                                           RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                                           &key,
                                           &series,
                                           REDISMODULE_READ);
        if (!status) {
            RedisModule_Log(ctx,
                            "warning",
                            "couldn't open key or key is not a Timeseries. key=%.*s",
                            currentKeyLen,
                            currentKey);
            continue;
        }
        RedisModule_ReplyWithArray(ctx, 3);
        RedisModule_ReplyWithStringBuffer(ctx, currentKey, currentKeyLen);
        if (withlabels_location >= 0) {
            ReplyWithSeriesLabels(ctx, series);
        } else {
            RedisModule_ReplyWithArray(ctx, 0);
        }
        ReplyWithSeriesLastDatapoint(ctx, series);
        replylen++;
        RedisModule_CloseKey(key);
    }
    RedisModule_ReplySetArrayLength(ctx, replylen);
    RedisModule_DictIteratorStop(iter);
    QueryPredicateList_Free(queries);
    return REDISMODULE_OK;
}

int TSDB_delete(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    Series *series;
    RedisModuleKey *key;
    const int status = GetSeries(ctx, argv[1], &key, &series, REDISMODULE_READ);
    if (!status) {
        return REDISMODULE_ERR;
    }

    RangeArgs args = { 0 };
    if (parseRangeArguments(ctx, 2, argv, argc, (timestamp_t)0, &args) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    SeriesDelRange(series, args.startTimestamp, args.endTimestamp);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int NotifyCallback(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    if (strcasecmp(event, "del") == 0) {
        CleanLastDeletedSeries(key);
    }

    if (strcasecmp(event, "restore") == 0) {
        RestoreKey(ctx, key);
    }

    if (strcasecmp(event, "rename_from") == 0) {
        RenameSeriesFrom(ctx, key);
    }

    if (strcasecmp(event, "rename_to") == 0) {
        RenameSeriesTo(ctx, key);
    }

    return REDISMODULE_OK;
}

void module_loaded(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    if (subevent != REDISMODULE_SUBEVENT_MODULE_LOADED) {
        return;
    }
    RedisModuleModuleChange *moduleChange = (RedisModuleModuleChange *)data;
    if (strcasecmp(moduleChange->module_name, "rg") == 0) {
        register_rg(ctx);
    }
}

/*
module loading function, possible arguments:
COMPACTION_POLICY - compaction policy from parse_policies,h
RETENTION_POLICY - long that represents the retention in milliseconds
MAX_SAMPLE_PER_CHUNK - how many samples per chunk
example:
redis-server --loadmodule ./redistimeseries.so COMPACTION_POLICY
"max:1m:1d;min:10s:1h;avg:2h:10d;avg:3d:100d" RETENTION_POLICY 3600 MAX_SAMPLE_PER_CHUNK 1024
*/
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "timeseries", REDISTIMESERIES_MODULE_VERSION, REDISMODULE_APIVER_1) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx,
                    "notice",
                    "RedisTimeSeries version %d, git_sha=%s",
                    REDISTIMESERIES_MODULE_VERSION,
                    REDISTIMESERIES_GIT_SHA);

    RTS_GetRedisVersion();
    RedisModule_Log(ctx,
                    "notice",
                    "Redis version found by RedisTimeSeries : %d.%d.%d - %s",
                    RTS_currVersion.redisMajorVersion,
                    RTS_currVersion.redisMinorVersion,
                    RTS_currVersion.redisPatchVersion,
                    RTS_IsEnterprise() ? "enterprise" : "oss");
    if (RTS_IsEnterprise()) {
        RedisModule_Log(ctx,
                        "notice",
                        "Redis Enterprise version found by RedisTimeSeries : %d.%d.%d-%d",
                        RTS_RlecMajorVersion,
                        RTS_RlecMinorVersion,
                        RTS_RlecPatchVersion,
                        RTS_RlecBuild);
    }

    if (RTS_CheckSupportedVestion() != REDISMODULE_OK) {
        RedisModule_Log(ctx,
                        "warning",
                        "Redis version is to old, please upgrade to redis "
                        "%d.%d.%d and above.",
                        RTS_minSupportedVersion.redisMajorVersion,
                        RTS_minSupportedVersion.redisMinorVersion,
                        RTS_minSupportedVersion.redisPatchVersion);
        return REDISMODULE_ERR;
    }

    if (ReadConfig(ctx, argv, argc) == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    // ignore errors from redis gears registration, this can fail if the module is not loaded.
    register_rg(ctx);

    RedisModuleTypeMethods tm = { .version = REDISMODULE_TYPE_METHOD_VERSION,
                                  .rdb_load = series_rdb_load,
                                  .rdb_save = series_rdb_save,
                                  .aof_rewrite = RMUtil_DefaultAofRewrite,
                                  .mem_usage = SeriesMemUsage,
                                  .free = FreeSeries };

    SeriesType = RedisModule_CreateDataType(ctx, "TSDB-TYPE", TS_SIZE_RDB_VER, &tm);
    if (SeriesType == NULL)
        return REDISMODULE_ERR;
    IndexInit();
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.create", TSDB_create);
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.alter", TSDB_alter);
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.createrule", TSDB_createRule);
    RMUtil_RegisterWriteCmd(ctx, "ts.deleterule", TSDB_deleteRule);
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.add", TSDB_add);
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.incrby", TSDB_incrby);
    RMUtil_RegisterWriteDenyOOMCmd(ctx, "ts.decrby", TSDB_incrby);
    RMUtil_RegisterReadCmd(ctx, "ts.range", TSDB_range);
    RMUtil_RegisterReadCmd(ctx, "ts.revrange", TSDB_revrange);

    if (RedisModule_CreateCommand(ctx, "ts.queryindex", TSDB_queryindex, "readonly", 0, 0, 0) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RMUtil_RegisterReadCmd(ctx, "ts.info", TSDB_info);
    RMUtil_RegisterReadCmd(ctx, "ts.get", TSDB_get);
    RMUtil_RegisterWriteCmd(ctx, "ts.del", TSDB_delete);

    if (RedisModule_CreateCommand(ctx, "ts.madd", TSDB_madd, "write deny-oom", 1, -1, 3) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "ts.mrange", TSDB_mrange, "readonly", 0, 0, 0) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "ts.mrevrange", TSDB_mrevrange, "readonly", 0, 0, 0) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "ts.mget", TSDB_mget, "readonly", 0, 0, 0) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_GENERIC, NotifyCallback);

    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ModuleChange, module_loaded);

    return REDISMODULE_OK;
}
