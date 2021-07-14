/*
 * Copyright 2018-2021 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "consts.h"
#include "query_language.h"
#include "tsdb.h"

#ifndef REDISTIMESERIES_RESULTSET_H
#define REDISTIMESERIES_RESULTSET_H

/* Incomplete structures for compiler checks but opaque access. */
typedef struct TS_ResultSet TS_ResultSet;
typedef struct TS_GroupList TS_GroupList;

TS_ResultSet *ResultSet_Create();

int ResultSet_GroupbyLabel(TS_ResultSet *r, const char *label);

int ResultSet_SetLabelKey(TS_ResultSet *r, const char *labelKey);

int ResultSet_SetLabelValue(TS_ResultSet *r, const char *label);

int ResultSet_ApplyReducer(TS_ResultSet *r,
                           RangeArgs *args,
                           MultiSeriesReduceOp reducerOp,
                           bool reverse);

int parseMultiSeriesReduceOp(const char *reducerstr, MultiSeriesReduceOp *reducerOp);

int ResultSet_AddSerie(TS_ResultSet *r, Series *serie, const char *name);

void replyResultSet(RedisModuleCtx *ctx,
                    TS_ResultSet *r,
                    bool withlabels,
                    RedisModuleString *limitLabels[],
                    ushort limitLabelsSize,
                    RangeArgs *args,
                    bool rev);

void ResultSet_Free(TS_ResultSet *r);

int MultiSerieReduce(Series *dest,
                     Series *source,
                     MultiSeriesReduceOp op,
                     RangeArgs *args,
                     bool reverse);

#endif // REDISTIMESERIES_RESULTSET_H
