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
                        RedisModuleString *limitLabels[],
                        ushort limitLabelsSize,
                        RangeArgs *args,
                        bool rev);

int ReplySeriesRange(RedisModuleCtx *ctx, Series *series, RangeArgs *args, bool rev);

void ReplyWithSeriesLabels(RedisModuleCtx *ctx, const Series *series);
void ReplyWithSeriesLabelsWithLimit(RedisModuleCtx *ctx,
                                    const Series *series,
                                    RedisModuleString **limitLabels,
                                    ushort limitLabelsSize);
void ReplyWithSeriesLabelsWithLimitC(RedisModuleCtx *ctx,
                                     const Series *series,
                                     char **limitLabels,
                                     ushort limitLabelsSize);

void ReplyWithSample(RedisModuleCtx *ctx, u_int64_t timestamp, double value);

void ReplyWithSeriesLastDatapoint(RedisModuleCtx *ctx, const Series *series);

#endif // REDISTIMESERIES_REPLY_H
