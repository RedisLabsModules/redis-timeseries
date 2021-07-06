/*
 * Copyright 2018-2021 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "reply.h"

#include "fpconv.h"
#include "query_language.h"
#include "redismodule.h"
#include "series_iterator.h"
#include "tsdb.h"

#include "rmutil/alloc.h"

int ReplySeriesArrayPos(RedisModuleCtx *ctx,
                        Series *s,
                        bool withlabels,
                        RangeArgs *args,
                        bool rev) {
    RedisModule_ReplyWithArray(ctx, 3);
    RedisModule_ReplyWithString(ctx, s->keyName);
    if (withlabels) {
        ReplyWithSeriesLabels(ctx, s);
    } else {
        RedisModule_ReplyWithArray(ctx, 0);
    }
    ReplySeriesRange(ctx, s, args, rev);
    return REDISMODULE_OK;
}

int ReplySeriesRange(RedisModuleCtx *ctx, Series *series, RangeArgs *args, bool reverse) {
    Sample sample;
    long long arraylen = 0;

    AggregationClass *aggObject = args->aggregationArgs.aggregationClass;

    if (aggObject && SeriesIsBlob(series))
        aggObject = BlobAggClass(aggObject);

    // In case a retention is set shouldn't return chunks older than the retention
    // TODO: move to parseRangeArguments(?) or to iterator
    if (series->retentionTime) {
        args->startTimestamp =
            series->lastTimestamp > series->retentionTime
                ? max(args->startTimestamp, series->lastTimestamp - series->retentionTime)
                : args->startTimestamp;
        // if new start_ts > end_ts, there are no results to return
        if (args->startTimestamp > args->endTimestamp) {
            return RedisModule_ReplyWithArray(ctx, 0);
        }
    }

    AbstractIterator *iter = SeriesQuery(series, args, reverse);

    bool replyIsBlob = (aggObject == NULL && SeriesIsBlob(series)) || aggClassIsBlob(aggObject);

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    while (iter->GetNext(iter, &sample) == CR_OK && (args->count == -1 || arraylen < args->count)) {
        ReplyWithSample(ctx, replyIsBlob, sample.timestamp, sample.value);
        arraylen++;
    }
    iter->Close(iter);

    RedisModule_ReplySetArrayLength(ctx, arraylen);
    return REDISMODULE_OK;
}

void ReplyWithSeriesLabels(RedisModuleCtx *ctx, const Series *series) {
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
void ReplyWithSample(RedisModuleCtx *ctx, bool isBlob, u_int64_t timestamp, SampleValue value) {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, timestamp);

    if (!isBlob) {
        char buf[MAX_VAL_LEN];
        int str_len = fpconv_dtoa(VALUE_DOUBLE(&value), buf);
        buf[str_len] = '\0';
        RedisModule_ReplyWithSimpleString(ctx, buf);
    } else {
        TSBlob *blob = VALUE_BLOB(&value);
        RedisModule_ReplyWithStringBuffer(ctx, blob->data, blob->len);
    }
}

void ReplyWithSeriesLastDatapoint(RedisModuleCtx *ctx, const Series *series) {
    if (SeriesGetNumSamples(series) == 0) {
        RedisModule_ReplyWithArray(ctx, 0);
    } else {
        ReplyWithSample(ctx, SeriesIsBlob(series), series->lastTimestamp, series->lastValue);
    }
}
