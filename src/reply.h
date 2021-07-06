/*
 * Copyright 2018-2021 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "generic_chunk.h"
#include "query_language.h"
#include "redismodule.h"
#include "tsdb.h"

#ifndef REDISTIMESERIES_REPLY_H
#define REDISTIMESERIES_REPLY_H

int ReplySeriesArrayPos(RedisModuleCtx *ctx,
                        Series *series,
                        bool withlabels,
                        RangeArgs *args,
                        bool rev);

int ReplySeriesRange(RedisModuleCtx *ctx, Series *series, RangeArgs *args, bool rev);

void ReplyWithSeriesLabels(RedisModuleCtx *ctx, const Series *series);

void ReplyWithSample(RedisModuleCtx *ctx, bool isBlob, u_int64_t timestamp, SampleValue value);

void ReplyWithSeriesLastDatapoint(RedisModuleCtx *ctx, const Series *series);

#endif // REDISTIMESERIES_REPLY_H
