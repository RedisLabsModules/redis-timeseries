/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "module.h"

#include "common.h"
#include "compaction.h"
#include "config.h"
#include "indexer.h"
#include "rdb.h"
#include "redismodule.h"
#include "tsdb.h"
#include "version.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include <rmutil/alloc.h>

#ifndef REDISTIMESERIES_GIT_SHA
#define REDISTIMESERIES_GIT_SHA "unknown"
#endif

RedisModuleType *SeriesType;

static int ReplySeriesRange(RedisModuleCtx *ctx,
                            Series *series,
                            api_timestamp_t start_ts,
                            api_timestamp_t end_ts,
                            AggregationClass *aggObject,
                            int64_t time_delta,
                            long long maxResults,
                            bool rev);

static void ReplyWithSeriesLabels(RedisModuleCtx *ctx, const Series *series);
static void ReplyWithSeriesLastDatapoint(RedisModuleCtx *ctx, const Series *series);

static int parseLabelsFromArgs(RedisModuleString **argv,
                               int argc,
                               size_t *label_count,
                               Label **labels) {
    int pos = RMUtil_ArgIndex("LABELS", argv, argc);
    int first_label_pos = pos + 1;
    Label *labelsResult = NULL;
    *label_count = 0;
    if (pos < 0) {
        *labels = NULL;
        return REDISMODULE_OK;
    }
    *label_count = (size_t)(max(0, (argc - first_label_pos) / 2));
    if (*label_count > 0) {
        labelsResult = malloc(sizeof(Label) * (*label_count));
        for (int i = 0; i < *label_count; i++) {
            RedisModuleString *key = argv[first_label_pos + i * 2];
            RedisModuleString *value = argv[first_label_pos + i * 2 + 1];

            // Verify Label Key or Value are not empty strings
            size_t keyLen, valueLen;
            RedisModule_StringPtrLen(key, &keyLen);
            RedisModule_StringPtrLen(value, &valueLen);
            if (keyLen == 0 || valueLen == 0 ||
                strpbrk(RedisModule_StringPtrLen(value, NULL), "(),")) {
                FreeLabels(labelsResult, i); // need to release prior key values too
                return REDISMODULE_ERR;
            }

            labelsResult[i].key = RedisModule_CreateStringFromString(NULL, key);
            labelsResult[i].value = RedisModule_CreateStringFromString(NULL, value);
        };
    }
    *labels = labelsResult;
    return REDISMODULE_OK;
}

int GetSeries(RedisModuleCtx *ctx,
              RedisModuleString *keyName,
              RedisModuleKey **key,
              Series **series,
              int mode) {
    *key = RedisModule_OpenKey(ctx, keyName, mode);
    if (RedisModule_KeyType(*key) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(*key);
        RTS_ReplyGeneralError(ctx, "TSDB: the key does not exist");
        return FALSE;
    }
    if (RedisModule_ModuleTypeGetType(*key) != SeriesType) {
        RedisModule_CloseKey(*key);
        RTS_ReplyGeneralError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return FALSE;
    }
    *series = RedisModule_ModuleTypeGetValue(*key);
    return TRUE;
}

static int parseCreateArgs(RedisModuleCtx *ctx,
                           RedisModuleString **argv,
                           int argc,
                           CreateCtx *cCtx) {
    cCtx->retentionTime = TSGlobalConfig.retentionPolicy;
    cCtx->chunkSizeBytes = TSGlobalConfig.chunkSizeBytes;
    cCtx->labelsCount = 0;
    if (parseLabelsFromArgs(argv, argc, &cCtx->labelsCount, &cCtx->labels) == REDISMODULE_ERR) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse LABELS");
        return REDISMODULE_ERR;
    }

    if (RMUtil_ArgIndex("RETENTION", argv, argc) > 0 &&
        RMUtil_ParseArgsAfter("RETENTION", argv, argc, "l", &cCtx->retentionTime) !=
            REDISMODULE_OK) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse RETENTION");
        return REDISMODULE_ERR;
    }

    if (cCtx->retentionTime < 0) {
        RedisModule_ReplyWithError(ctx, "TSDB: Couldn't parse RETENTION");
        return REDISMODULE_ERR;
    }

    if (RMUtil_ArgIndex("CHUNK_SIZE", argv, argc) > 0 &&
        RMUtil_ParseArgsAfter("CHUNK_SIZE", argv, argc, "l", &cCtx->chunkSizeBytes) !=
            REDISMODULE_OK) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse CHUNK_SIZE");
        return REDISMODULE_ERR;
    }

    if (cCtx->chunkSizeBytes <= 0) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse CHUNK_SIZE");
        return REDISMODULE_ERR;
    }

    if (RMUtil_ArgIndex("UNCOMPRESSED", argv, argc) > 0) {
        cCtx->options |= SERIES_OPT_UNCOMPRESSED;
    }

    return REDISMODULE_OK;
}

static int _parseAggregationArgs(RedisModuleCtx *ctx,
                                 RedisModuleString **argv,
                                 int argc,
                                 api_timestamp_t *time_delta,
                                 int *agg_type) {
    RedisModuleString *aggTypeStr = NULL;
    int offset = RMUtil_ArgIndex("AGGREGATION", argv, argc);
    if (offset > 0) {
        long long temp_time_delta = 0;
        if (RMUtil_ParseArgs(argv, argc, offset + 1, "sl", &aggTypeStr, &temp_time_delta) !=
            REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse AGGREGATION");
            return TSDB_ERROR;
        }

        if (!aggTypeStr) {
            RTS_ReplyGeneralError(ctx, "TSDB: Unknown aggregation type");
            return TSDB_ERROR;
        }

        *agg_type = RMStringLenAggTypeToEnum(aggTypeStr);

        if (*agg_type < 0 || *agg_type >= TS_AGG_TYPES_MAX) {
            RTS_ReplyGeneralError(ctx, "TSDB: Unknown aggregation type");
            return TSDB_ERROR;
        }

        if (temp_time_delta <= 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: timeBucket must be greater than zero");
            return TSDB_ERROR;
        } else {
            *time_delta = (api_timestamp_t)temp_time_delta;
        }

        return TSDB_OK;
    }

    return TSDB_NOTEXISTS;
}

static int parseAggregationArgs(RedisModuleCtx *ctx,
                                RedisModuleString **argv,
                                int argc,
                                api_timestamp_t *time_delta,
                                AggregationClass **agg_object) {
    int agg_type;
    int result = _parseAggregationArgs(ctx, argv, argc, time_delta, &agg_type);
    if (result == TSDB_OK) {
        *agg_object = GetAggClass(agg_type);
        if (*agg_object == NULL) {
            RTS_ReplyGeneralError(ctx, "TSDB: Failed to retrieve aggregation class");
            return TSDB_ERROR;
        }
        return TSDB_OK;
    } else {
        return result;
    }
}

static int parseRangeArguments(RedisModuleCtx *ctx,
                               Series *series,
                               int start_index,
                               RedisModuleString **argv,
                               api_timestamp_t *start_ts,
                               api_timestamp_t *end_ts) {
    size_t start_len;
    const char *start = RedisModule_StringPtrLen(argv[start_index], &start_len);
    if (strcmp(start, "-") == 0) {
        *start_ts = 0;
    } else {
        if (RedisModule_StringToLongLong(argv[start_index], (long long int *)start_ts) !=
            REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: wrong fromTimestamp");
            return REDISMODULE_ERR;
        }
    }

    size_t end_len;
    const char *end = RedisModule_StringPtrLen(argv[start_index + 1], &end_len);
    if (strcmp(end, "+") == 0) {
        *end_ts = series->lastTimestamp;
    } else {
        if (RedisModule_StringToLongLong(argv[start_index + 1], (long long int *)end_ts) !=
            REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: wrong toTimestamp");
            return REDISMODULE_ERR;
        }
    }

    return REDISMODULE_OK;
}

static int parseCountArgument(RedisModuleCtx *ctx,
                              RedisModuleString **argv,
                              int argc,
                              long long *count) {
    int offset = RMUtil_ArgIndex("COUNT", argv, argc);
    if (offset > 0) {
        if (offset + 1 == argc) {
            RTS_ReplyGeneralError(ctx, "TSDB: COUNT argument is missing");
            return TSDB_ERROR;
        }
        if (strcasecmp(RedisModule_StringPtrLen(argv[offset - 1], NULL), "AGGREGATION") == 0) {
            int second_offset =
                offset + 1 + RMUtil_ArgIndex("COUNT", argv + offset + 1, argc - offset - 1);
            if (offset == second_offset) {
                return TSDB_OK;
            }
            offset = second_offset;
        }
        if (RedisModule_StringToLongLong(argv[offset + 1], count) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse COUNT");
            return TSDB_ERROR;
        }
    }
    return TSDB_OK;
}

int TSDB_info(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
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

    RedisModule_ReplyWithArray(ctx, 10 * 2);

    long long skippedSamples;
    long long firstTimestamp = getFirstValidTimestamp(series, &skippedSamples);

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
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

static void ReplyWithSeriesLabels(RedisModuleCtx *ctx, const Series *series) {
    RedisModule_ReplyWithArray(ctx, series->labelsCount);
    for (int i = 0; i < series->labelsCount; i++) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithString(ctx, series->labels[i].key);
        RedisModule_ReplyWithString(ctx, series->labels[i].value);
    }
}

// double string presentation requires 15 digit integers +
// '.' + "e+" or "e-" + 3 digits of exponent
#define MAX_VAL_LEN 24
static void ReplyWithSample(RedisModuleCtx *ctx, u_int64_t timestamp, double value) {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, timestamp);
    char buf[MAX_VAL_LEN];
    snprintf(buf, MAX_VAL_LEN, "%.15g", value);
    RedisModule_ReplyWithSimpleString(ctx, buf);
}

void ReplyWithSeriesLastDatapoint(RedisModuleCtx *ctx, const Series *series) {
    if (SeriesGetNumSamples(series) == 0) {
        RedisModule_ReplyWithArray(ctx, 0);
    } else {
        ReplyWithSample(ctx, series->lastTimestamp, series->lastValue);
    }
}

int parseLabelListFromArgs(RedisModuleCtx *ctx,
                           RedisModuleString **argv,
                           int start,
                           int query_count,
                           QueryPredicate *queries) {
    QueryPredicate *query = queries;
    for (int i = start; i < start + query_count; i++) {
        size_t _s;
        const char *str2 = RedisModule_StringPtrLen(argv[i], &_s);
        if (strstr(str2, "!=(") != NULL) { // order is important! Must be before "!=".
            query->type = LIST_NOTMATCH;
            if (parsePredicate(ctx, argv[i], query, "!=(") == TSDB_ERROR) {
                return TSDB_ERROR;
            }
        } else if (strstr(str2, "!=") != NULL) {
            query->type = NEQ;
            if (parsePredicate(ctx, argv[i], query, "!=") == TSDB_ERROR) {
                return TSDB_ERROR;
            }
            if (query->valueListCount == 0) {
                query->type = CONTAINS;
            }
        } else if (strstr(str2, "=(") != NULL) { // order is important! Must be before "=".
            query->type = LIST_MATCH;
            if (parsePredicate(ctx, argv[i], query, "=(") == TSDB_ERROR) {
                return TSDB_ERROR;
            }
        } else if (strstr(str2, "=") != NULL) {
            query->type = EQ;
            if (parsePredicate(ctx, argv[i], query, "=") == TSDB_ERROR) {
                return TSDB_ERROR;
            }
            if (query->valueListCount == 0) {
                query->type = NCONTAINS;
            }
        } else {
            return TSDB_ERROR;
        }
        query++;
    }
    return TSDB_OK;
}

int TSDB_queryindex(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    int query_count = argc - 1;

    QueryPredicate *queries = RedisModule_PoolAlloc(ctx, sizeof(QueryPredicate) * query_count);
    if (parseLabelListFromArgs(ctx, argv, 1, query_count, queries) == TSDB_ERROR) {
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, (size_t)query_count, EQ) +
            CountPredicateType(queries, (size_t)query_count, LIST_MATCH) ==
        0) {
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    RedisModuleDict *result = QueryIndex(ctx, queries, query_count);

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

    return REDISMODULE_OK;
}

int TSDB_generic_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
    RedisModule_AutoMemory(ctx);

    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    api_timestamp_t start_ts, end_ts;
    api_timestamp_t time_delta = 0;
    Series fake_series = { 0 };
    fake_series.lastTimestamp = LLONG_MAX;
    if (parseRangeArguments(ctx, &fake_series, 1, argv, &start_ts, &end_ts) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    AggregationClass *aggObject = NULL;
    const int aggregationResult = parseAggregationArgs(ctx, argv, argc, &time_delta, &aggObject);
    if (aggregationResult == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    const int filter_location = RMUtil_ArgIndex("FILTER", argv, argc);
    if (filter_location == -1) {
        return RedisModule_WrongArity(ctx);
    }

    long long count = -1;
    if (parseCountArgument(ctx, argv, argc, &count) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    const size_t query_count = argc - 1 - filter_location;
    const int withlabels_location = RMUtil_ArgIndex("WITHLABELS", argv, argc);
    QueryPredicate *queries = RedisModule_PoolAlloc(ctx, sizeof(QueryPredicate) * query_count);
    if (parseLabelListFromArgs(ctx, argv, filter_location + 1, query_count, queries) ==
        TSDB_ERROR) {
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, (size_t)query_count, EQ) +
            CountPredicateType(queries, (size_t)query_count, LIST_MATCH) ==
        0) {
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    RedisModuleDict *result = QueryIndex(ctx, queries, query_count);

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const int status = GetSeries(ctx,
                                     RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                                     &key,
                                     &series,
                                     REDISMODULE_READ);
        if (!status) {
            RedisModule_Log(
                ctx, "warning", "couldn't open key or key is not a Timeseries. key=%s", currentKey);
            continue;
        }
        RedisModule_ReplyWithArray(ctx, 3);
        RedisModule_ReplyWithStringBuffer(ctx, currentKey, currentKeyLen);
        if (withlabels_location >= 0) {
            ReplyWithSeriesLabels(ctx, series);
        } else {
            RedisModule_ReplyWithArray(ctx, 0);
        }
        ReplySeriesRange(ctx, series, start_ts, end_ts, aggObject, time_delta, count, rev);
        replylen++;
        RedisModule_CloseKey(key);
    }
    RedisModule_DictIteratorStop(iter);
    RedisModule_ReplySetArrayLength(ctx, replylen);

    return REDISMODULE_OK;
}

int TSDB_mrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_mrange(ctx, argv, argc, false);
}

int TSDB_mrevrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_mrange(ctx, argv, argc, true);
}

int TSDB_generic_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool rev) {
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

    api_timestamp_t start_ts, end_ts;
    api_timestamp_t time_delta = 0;
    if (parseRangeArguments(ctx, series, 2, argv, &start_ts, &end_ts) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    long long count = -1;
    if (parseCountArgument(ctx, argv, argc, &count) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    AggregationClass *aggObject = NULL;
    int aggregationResult = parseAggregationArgs(ctx, argv, argc, &time_delta, &aggObject);
    if (aggregationResult == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    ReplySeriesRange(ctx, series, start_ts, end_ts, aggObject, time_delta, count, rev);

    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

int TSDB_range(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, false);
}

int TSDB_revrange(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return TSDB_generic_range(ctx, argv, argc, true);
}

int ReplySeriesRange(RedisModuleCtx *ctx,
                     Series *series,
                     api_timestamp_t start_ts,
                     api_timestamp_t end_ts,
                     AggregationClass *aggObject,
                     int64_t time_delta,
                     long long maxResults,
                     bool rev) {
    Sample sample;
    void *context = NULL;
    long long arraylen = 0;
    timestamp_t last_agg_timestamp;

    // In case a retention is set shouldn't return chunks older than the retention
    // TODO: move to parseRangeArguments(?)
    if (series->retentionTime) {
        start_ts = series->lastTimestamp > series->retentionTime
                       ? max(start_ts, series->lastTimestamp - series->retentionTime)
                       : start_ts;
        // if new start_ts > end_ts, there are no results to return
        if (start_ts > end_ts) {
            return RedisModule_ReplyWithArray(ctx, 0);
        }
    }

    SeriesIterator iterator = SeriesQuery(series, start_ts, end_ts, rev);
    if (iterator.series == NULL) {
        return RedisModule_ReplyWithArray(ctx, 0);
    }

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    if (aggObject == NULL) {
        // No aggregation
        while (SeriesIteratorGetNext(&iterator, &sample) == CR_OK &&
               (maxResults == -1 || arraylen < maxResults)) {
            ReplyWithSample(ctx, sample.timestamp, sample.value);
            arraylen++;
        }
    } else {
        bool firstSample = TRUE;
        context = aggObject->createContext();
        ChunkFuncs *funcs = GetChunkFuncs(iterator.currentChunk);
        // setting the first timestamp of the aggregation
        timestamp_t init_ts = (rev == false) ? funcs->GetFirstTimestamp(iterator.currentChunk)
                                             : funcs->GetLastTimestamp(iterator.currentChunk);
        last_agg_timestamp = init_ts - (init_ts % time_delta);

        while (SeriesIteratorGetNext(&iterator, &sample) == CR_OK &&
               (maxResults == -1 || arraylen < maxResults)) {
            if ((iterator.reverse == false &&
                 sample.timestamp >= last_agg_timestamp + time_delta) ||
                (iterator.reverse == true && sample.timestamp < last_agg_timestamp)) {
                if (firstSample == FALSE) {
                    double value;
                    if (aggObject->finalize(context, &value) == TSDB_OK) {
                        ReplyWithSample(ctx, last_agg_timestamp, value);
                        aggObject->resetContext(context);
                        arraylen++;
                    }
                }
                last_agg_timestamp = sample.timestamp - (sample.timestamp % time_delta);
            }
            firstSample = FALSE;
            aggObject->appendValue(context, sample.value);
        }
    }
    SeriesIteratorClose(&iterator);

    if (aggObject != NULL) {
        if (arraylen != maxResults) {
            // reply last bucket of data
            double value;
            if (aggObject->finalize(context, &value) == TSDB_OK) {
                ReplyWithSample(ctx, last_agg_timestamp, value);
                aggObject->resetContext(context);
                arraylen++;
            }
        }
        aggObject->freeContext(context);
    }

    RedisModule_ReplySetArrayLength(ctx, arraylen);
    return REDISMODULE_OK;
}

static void handleCompaction(RedisModuleCtx *ctx,
                             Series *series,
                             CompactionRule *rule,
                             api_timestamp_t timestamp,
                             double value) {
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

        double aggVal;
        if (rule->aggClass->finalize(rule->aggContext, &aggVal) == TSDB_OK) {
            SeriesAddSample(destSeries, rule->startCurrentTimeBucket, aggVal);
        }
        rule->aggClass->resetContext(rule->aggContext);
        rule->startCurrentTimeBucket = currentTimestamp;
        RedisModule_CloseKey(key);
    }
    rule->aggClass->appendValue(rule->aggContext, value);
}

static int internalAdd(RedisModuleCtx *ctx,
                       Series *series,
                       api_timestamp_t timestamp,
                       double value) {
    timestamp_t lastTS = series->lastTimestamp;
    uint64_t retention = series->retentionTime;
    // ensure inside retention period.
    if (retention && timestamp < lastTS && retention < lastTS - timestamp) {
        RTS_ReplyGeneralError(ctx, "TSDB: Timestamp is older than retention");
        return REDISMODULE_ERR;
    }

    if (timestamp <= series->lastTimestamp && series->totalSamples != 0) {
        if (SeriesUpsertSample(series, timestamp, value) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Error at upsert");
            return REDISMODULE_ERR;
        }
    } else {
        if (SeriesAddSample(series, timestamp, value) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Error at add");
            return REDISMODULE_ERR;
        }
        // handle compaction rules
        CompactionRule *rule = series->rules;
        while (rule != NULL) {
            handleCompaction(ctx, series, rule, timestamp, value);
            rule = rule->nextRule;
        }
    }
    RedisModule_ReplyWithLongLong(ctx, timestamp);
    return REDISMODULE_OK;
}

static inline int add(RedisModuleCtx *ctx,
                      RedisModuleString *keyName,
                      RedisModuleString *timestampStr,
                      RedisModuleString *valueStr,
                      RedisModuleString **argv,
                      int argc) {
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ | REDISMODULE_WRITE);
    double value;
    api_timestamp_t timestamp;
    if ((RedisModule_StringToDouble(valueStr, &value) != REDISMODULE_OK))
        return RTS_ReplyGeneralError(ctx, "TSDB: invalid value");

    if ((RedisModule_StringToLongLong(timestampStr, (long long int *)&timestamp) !=
         REDISMODULE_OK)) {
        // if timestamp is "*", take current time (automatic timestamp)
        if (RMUtil_StringEqualsC(timestampStr, "*"))
            timestamp = (u_int64_t)RedisModule_Milliseconds();
        else
            return RTS_ReplyGeneralError(ctx, "TSDB: invalid timestamp");
    }

    Series *series = NULL;

    if (argv != NULL && RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        // the key doesn't exist, lets check we have enough information to create one
        CreateCtx cCtx = { 0 };
        if (parseCreateArgs(ctx, argv, argc, &cCtx) != REDISMODULE_OK) {
            return REDISMODULE_ERR;
        }

        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    } else if (RedisModule_ModuleTypeGetType(key) != SeriesType) {
        return RTS_ReplyGeneralError(ctx, "TSDB: the key is not a TSDB key");
    } else {
        series = RedisModule_ModuleTypeGetValue(key);
    }
    int rv = internalAdd(ctx, series, timestamp, value);
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

        CreateTsKey(ctx, keyName, &cCtx, &series, &key);
        SeriesCreateRulesFromGlobalConfig(ctx, keyName, series, cCtx.labels, cCtx.labelsCount);
    }

    series = RedisModule_ModuleTypeGetValue(key);

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

    double result = series->lastValue;
    RMUtil_StringToLower(argv[0]);
    if (RMUtil_StringEqualsC(argv[0], "ts.incrby")) {
        result += incrby;
    } else {
        result -= incrby;
    }

    int rv = internalAdd(ctx, series, currentUpdatedTime, result);
    RedisModule_ReplicateVerbatim(ctx);
    RedisModule_CloseKey(key);
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
    QueryPredicate *queries = RedisModule_PoolAlloc(ctx, sizeof(QueryPredicate) * query_count);
    if (parseLabelListFromArgs(ctx, argv, filter_location + 1, query_count, queries) ==
        TSDB_ERROR) {
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, (size_t)query_count, EQ) +
            CountPredicateType(queries, (size_t)query_count, LIST_MATCH) ==
        0) {
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    RedisModuleDict *result = QueryIndex(ctx, queries, query_count);
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
    char *currentKey;
    size_t currentKeyLen;
    long long replylen = 0;
    Series *series;
    while ((currentKey = RedisModule_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        RedisModuleKey *key;
        const int status = GetSeries(ctx,
                                     RedisModule_CreateString(ctx, currentKey, currentKeyLen),
                                     &key,
                                     &series,
                                     REDISMODULE_READ);
        if (!status) {
            RedisModule_Log(
                ctx, "warning", "couldn't open key or key is not a Timeseries. key=%s", currentKey);
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
    return REDISMODULE_OK;
}

int NotifyCallback(RedisModuleCtx *original_ctx,
                   int type,
                   const char *event,
                   RedisModuleString *key) {
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModule_AutoMemory(ctx);

    if (strcasecmp(event, "del") == 0) {
        CleanLastDeletedSeries(ctx, key);
    }

    RedisModule_FreeThreadSafeContext(ctx);

    return REDISMODULE_OK;
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
    RMUtil_RegisterReadCmd(ctx, "ts.queryindex", TSDB_queryindex);
    RMUtil_RegisterReadCmd(ctx, "ts.info", TSDB_info);
    RMUtil_RegisterReadCmd(ctx, "ts.get", TSDB_get);

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

    return REDISMODULE_OK;
}
