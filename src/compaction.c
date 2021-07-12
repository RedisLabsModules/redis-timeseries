/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "compaction.h"

#include <ctype.h>
#include <float.h> // DBL_MAX, DBL_MIN
#include <math.h>  // sqrt
#include <string.h>
#include <rmutil/alloc.h>

typedef struct RangeContext
{
    double minValue;
    double maxValue;
    char isResetted;
} RangeContext;

typedef struct SingleValueContext
{
    double value;
    char isResetted;
} SingleValueContext;

typedef struct AvgContext
{
    double val;
    double cnt;
} AvgContext;

typedef struct StdContext
{
    double sum;
    double sum_2; // sum of (values^2)
    u_int64_t cnt;
} StdContext;

void *SingleValueCreateContext() {
    SingleValueContext *context = (SingleValueContext *)malloc(sizeof(SingleValueContext));
    context->value = 0;
    context->isResetted = TRUE;
    return context;
}

void SingleValueReset(void *contextPtr) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = 0;
    context->isResetted = TRUE;
}

int SingleValueFinalize(void *contextPtr, double *val) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted == true) {
        return TSDB_ERROR;
    }
    *val = context->value;
    return TSDB_OK;
}

void SingleValueWriteContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    RedisModule_SaveDouble(io, context->value);
}

void SingleValueReadContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = RedisModule_LoadDouble(io);
}

void *AvgCreateContext() {
    AvgContext *context = (AvgContext *)malloc(sizeof(AvgContext));
    context->cnt = 0;
    context->val = 0;
    return context;
}

void AvgAddValue(void *contextPtr, double value) {
    AvgContext *context = (AvgContext *)contextPtr;
    context->val += value;
    context->cnt++;
}

int AvgFinalize(void *contextPtr, double *value) {
    AvgContext *context = (AvgContext *)contextPtr;
    if (context->cnt == 0)
        return TSDB_ERROR;
    *value = context->val / context->cnt;
    return TSDB_OK;
}

void AvgReset(void *contextPtr) {
    AvgContext *context = (AvgContext *)contextPtr;
    context->val = 0;
    context->cnt = 0;
}

void AvgWriteContext(void *contextPtr, RedisModuleIO *io) {
    AvgContext *context = (AvgContext *)contextPtr;
    RedisModule_SaveDouble(io, context->val);
    RedisModule_SaveDouble(io, context->cnt);
}

void AvgReadContext(void *contextPtr, RedisModuleIO *io) {
    AvgContext *context = (AvgContext *)contextPtr;
    context->val = RedisModule_LoadDouble(io);
    context->cnt = RedisModule_LoadDouble(io);
}

void *StdCreateContext() {
    StdContext *context = (StdContext *)malloc(sizeof(StdContext));
    context->cnt = 0;
    context->sum = 0;
    context->sum_2 = 0;
    return context;
}

void StdAddValue(void *contextPtr, double value) {
    StdContext *context = (StdContext *)contextPtr;
    ++context->cnt;
    context->sum += value;
    context->sum_2 += value * value;
}

static inline double variance(double sum, double sum_2, double count) {
    if (count == 0) {
        return 0;
    }

    /*  var(X) = sum((x_i - E[X])^2)
     *  = sum(x_i^2) - 2 * sum(x_i) * E[X] + E^2[X] */
    return (sum_2 - 2 * sum * sum / count + pow(sum / count, 2) * count) / count;
}

int VarPopulationFinalize(void *contextPtr, double *value) {
    StdContext *context = (StdContext *)contextPtr;
    uint64_t count = context->cnt;
    if (count == 0) {
        return TSDB_ERROR;
    }
    *value = variance(context->sum, context->sum_2, count);
    return TSDB_OK;
}

int VarSamplesFinalize(void *contextPtr, double *value) {
    StdContext *context = (StdContext *)contextPtr;
    uint64_t count = context->cnt;
    if (count == 0) {
        return TSDB_ERROR;
    } else if (count == 1) {
        *value = 0;
    } else {
        *value = variance(context->sum, context->sum_2, count) * count / (count - 1);
    }
    return TSDB_OK;
}

int StdPopulationFinalize(void *contextPtr, double *value) {
    double val;
    int rv = VarPopulationFinalize(contextPtr, &val);
    if (rv != TSDB_OK) {
        return rv;
    }
    *value = sqrt(val);
    return TSDB_OK;
}

int StdSamplesFinalize(void *contextPtr, double *value) {
    double val;
    int rv = VarSamplesFinalize(contextPtr, &val);
    if (rv != TSDB_OK) {
        return rv;
    }
    *value = sqrt(val);
    return TSDB_OK;
}

void StdReset(void *contextPtr) {
    StdContext *context = (StdContext *)contextPtr;
    context->cnt = 0;
    context->sum = 0;
    context->sum_2 = 0;
}

void StdWriteContext(void *contextPtr, RedisModuleIO *io) {
    StdContext *context = (StdContext *)contextPtr;
    RedisModule_SaveDouble(io, context->sum);
    RedisModule_SaveDouble(io, context->sum_2);
    RedisModule_SaveUnsigned(io, context->cnt);
}

void StdReadContext(void *contextPtr, RedisModuleIO *io) {
    StdContext *context = (StdContext *)contextPtr;
    context->sum = RedisModule_LoadDouble(io);
    context->sum_2 = RedisModule_LoadDouble(io);
    context->cnt = RedisModule_LoadUnsigned(io);
}

void rm_free(void *ptr) {
    free(ptr);
}

static AggregationClass aggAvg = { .createContext = AvgCreateContext,
                                   .appendValue = AvgAddValue,
                                   .freeContext = rm_free,
                                   .finalize = AvgFinalize,
                                   .writeContext = AvgWriteContext,
                                   .readContext = AvgReadContext,
                                   .resetContext = AvgReset };

static AggregationClass aggStdP = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .freeContext = rm_free,
                                    .finalize = StdPopulationFinalize,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
                                    .resetContext = StdReset };

static AggregationClass aggStdS = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .freeContext = rm_free,
                                    .finalize = StdSamplesFinalize,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
                                    .resetContext = StdReset };

static AggregationClass aggVarP = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .freeContext = rm_free,
                                    .finalize = VarPopulationFinalize,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
                                    .resetContext = StdReset };

static AggregationClass aggVarS = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .freeContext = rm_free,
                                    .finalize = VarSamplesFinalize,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
                                    .resetContext = StdReset };

void MinAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted) {
        context->isResetted = FALSE;
        context->value = value;
    }
    if (value < context->value) {
        context->value = value;
    }
}

int MinFinalize(void *contextPtr, double *value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->value;
    return TSDB_OK;
}

void MinWriteContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    // backwards compatibility
    RedisModule_SaveDouble(io, context->value);
    // real write
    RedisModule_SaveDouble(io, context->value);
    RedisModule_SaveStringBuffer(io, &context->isResetted, 1);
}

void MinReadContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    // backwards compatibility
    RedisModule_LoadDouble(io);
    // real read
    context->value = RedisModule_LoadDouble(io);
    size_t len = 1;
    char *sb = RedisModule_LoadStringBuffer(io, &len);
    context->isResetted = sb[0];
    free(sb);
}

void MaxAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted) {
        context->isResetted = FALSE;
        context->value = value;
    }
    if (value > context->value) {
        context->value = value;
    }
}

int MaxFinalize(void *contextPtr, double *value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->value;
    return TSDB_OK;
}

void MaxWriteContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    RedisModule_SaveDouble(io, context->value);
    // backwards compatibility
    RedisModule_SaveDouble(io, context->value);
    RedisModule_SaveStringBuffer(io, &context->isResetted, 1);
}

void MaxReadContext(void *contextPtr, RedisModuleIO *io) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = RedisModule_LoadDouble(io);
    // backwards compatibility
    RedisModule_LoadDouble(io);
    size_t len = 1;
    char *sb = RedisModule_LoadStringBuffer(io, &len);
    context->isResetted = sb[0];
    free(sb);
}

void SumAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value += value;
    context->isResetted = FALSE;
}

void CountAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value++;
    context->isResetted = FALSE;
}

int CountFinalize(void *contextPtr, double *val) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    *val = context->value;
    return TSDB_OK;
}

void FirstAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted) {
        context->isResetted = FALSE;
        context->value = value;
    }
}

void LastAppendValue(void *contextPtr, double value) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = value;
    context->isResetted = FALSE;
}

void *RangeCreateContext() {
    RangeContext *context = (RangeContext *)malloc(sizeof(RangeContext));
    context->minValue = 0;
    context->maxValue = 0;
    context->isResetted = TRUE;
    return context;
}

void RangeAppendValue(void *contextPtr, double value) {
    RangeContext *context = (RangeContext *)contextPtr;
    if (context->isResetted) {
        context->isResetted = FALSE;
        context->maxValue = value;
        context->minValue = value;
    } else {
        if (value > context->maxValue) {
            context->maxValue = value;
        }
        if (value < context->minValue) {
            context->minValue = value;
        }
    }
}

int RangeFinalize(void *contextPtr, double *value) {
    RangeContext *context = (RangeContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->maxValue - context->minValue;
    return TSDB_OK;
}

void RangeReset(void *contextPtr) {
    RangeContext *context = (RangeContext *)contextPtr;
    context->maxValue = 0;
    context->minValue = 0;
    context->isResetted = TRUE;
}

void RangeWriteContext(void *contextPtr, RedisModuleIO *io) {
    RangeContext *context = (RangeContext *)contextPtr;
    RedisModule_SaveDouble(io, context->maxValue);
    RedisModule_SaveDouble(io, context->minValue);
    RedisModule_SaveStringBuffer(io, &context->isResetted, 1);
}

void RangeReadContext(void *contextPtr, RedisModuleIO *io) {
    RangeContext *context = (RangeContext *)contextPtr;
    size_t len = 1;
    context->maxValue = RedisModule_LoadDouble(io);
    context->minValue = RedisModule_LoadDouble(io);
    char *sb = RedisModule_LoadStringBuffer(io, &len);
    context->isResetted = sb[0];
    free(sb);
}

static AggregationClass aggMax = { .createContext = SingleValueCreateContext,
                                   .appendValue = MaxAppendValue,
                                   .freeContext = rm_free,
                                   .finalize = MaxFinalize,
                                   .writeContext = MaxWriteContext,
                                   .readContext = MaxReadContext,
                                   .resetContext = SingleValueReset };

static AggregationClass aggMin = { .createContext = SingleValueCreateContext,
                                   .appendValue = MinAppendValue,
                                   .freeContext = rm_free,
                                   .finalize = MinFinalize,
                                   .writeContext = MinWriteContext,
                                   .readContext = MinReadContext,
                                   .resetContext = SingleValueReset };

static AggregationClass aggSum = { .createContext = SingleValueCreateContext,
                                   .appendValue = SumAppendValue,
                                   .freeContext = rm_free,
                                   .finalize = SingleValueFinalize,
                                   .writeContext = SingleValueWriteContext,
                                   .readContext = SingleValueReadContext,
                                   .resetContext = SingleValueReset };

static AggregationClass aggCount = { .createContext = SingleValueCreateContext,
                                     .appendValue = CountAppendValue,
                                     .freeContext = rm_free,
                                     .finalize = CountFinalize,
                                     .writeContext = SingleValueWriteContext,
                                     .readContext = SingleValueReadContext,
                                     .resetContext = SingleValueReset };

static AggregationClass aggFirst = { .createContext = SingleValueCreateContext,
                                     .appendValue = FirstAppendValue,
                                     .freeContext = rm_free,
                                     .finalize = SingleValueFinalize,
                                     .writeContext = SingleValueWriteContext,
                                     .readContext = SingleValueReadContext,
                                     .resetContext = SingleValueReset };

static AggregationClass aggLast = { .createContext = SingleValueCreateContext,
                                    .appendValue = LastAppendValue,
                                    .freeContext = rm_free,
                                    .finalize = SingleValueFinalize,
                                    .writeContext = SingleValueWriteContext,
                                    .readContext = SingleValueReadContext,
                                    .resetContext = SingleValueReset };

static AggregationClass aggRange = { .createContext = RangeCreateContext,
                                     .appendValue = RangeAppendValue,
                                     .freeContext = rm_free,
                                     .finalize = RangeFinalize,
                                     .writeContext = RangeWriteContext,
                                     .readContext = RangeReadContext,
                                     .resetContext = RangeReset };

int StringAggTypeToEnum(const char *agg_type) {
    return StringLenAggTypeToEnum(agg_type, strlen(agg_type));
}

int RMStringLenAggTypeToEnum(RedisModuleString *aggTypeStr) {
    size_t str_len;
    const char *aggTypeCStr = RedisModule_StringPtrLen(aggTypeStr, &str_len);
    return StringLenAggTypeToEnum(aggTypeCStr, str_len);
}

int StringLenAggTypeToEnum(const char *agg_type, size_t len) {
    int result = TS_AGG_INVALID;
    char agg_type_lower[len];
    for (int i = 0; i < len; i++) {
        agg_type_lower[i] = tolower(agg_type[i]);
    }
    if (len == 3) {
        if (strncmp(agg_type_lower, "min", len) == 0 && len == 3) {
            result = TS_AGG_MIN;
        } else if (strncmp(agg_type_lower, "max", len) == 0) {
            result = TS_AGG_MAX;
        } else if (strncmp(agg_type_lower, "sum", len) == 0) {
            result = TS_AGG_SUM;
        } else if (strncmp(agg_type_lower, "avg", len) == 0) {
            result = TS_AGG_AVG;
        }
    } else if (len == 4) {
        if (strncmp(agg_type_lower, "last", len) == 0) {
            result = TS_AGG_LAST;
        }
    } else if (len == 5) {
        if (strncmp(agg_type_lower, "count", len) == 0) {
            result = TS_AGG_COUNT;
        } else if (strncmp(agg_type_lower, "range", len) == 0) {
            result = TS_AGG_RANGE;
        } else if (strncmp(agg_type_lower, "first", len) == 0) {
            result = TS_AGG_FIRST;
        } else if (strncmp(agg_type_lower, "std.p", len) == 0) {
            result = TS_AGG_STD_P;
        } else if (strncmp(agg_type_lower, "std.s", len) == 0) {
            result = TS_AGG_STD_S;
        } else if (strncmp(agg_type_lower, "var.p", len) == 0) {
            result = TS_AGG_VAR_P;
        } else if (strncmp(agg_type_lower, "var.s", len) == 0) {
            result = TS_AGG_VAR_S;
        }
    }
    return result;
}

const char *AggTypeEnumToString(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_MIN:
            return "MIN";
        case TS_AGG_MAX:
            return "MAX";
        case TS_AGG_SUM:
            return "SUM";
        case TS_AGG_AVG:
            return "AVG";
        case TS_AGG_STD_P:
            return "STD.P";
        case TS_AGG_STD_S:
            return "STD.S";
        case TS_AGG_VAR_P:
            return "VAR.P";
        case TS_AGG_VAR_S:
            return "VAR.S";
        case TS_AGG_COUNT:
            return "COUNT";
        case TS_AGG_FIRST:
            return "FIRST";
        case TS_AGG_LAST:
            return "LAST";
        case TS_AGG_RANGE:
            return "RANGE";
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return "Unknown";
}

AggregationClass *GetAggClass(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_MIN:
            return &aggMin;
        case TS_AGG_MAX:
            return &aggMax;
        case TS_AGG_AVG:
            return &aggAvg;
        case TS_AGG_STD_P:
            return &aggStdP;
        case TS_AGG_STD_S:
            return &aggStdS;
        case TS_AGG_VAR_P:
            return &aggVarP;
        case TS_AGG_VAR_S:
            return &aggVarS;
        case TS_AGG_SUM:
            return &aggSum;
        case TS_AGG_COUNT:
            return &aggCount;
        case TS_AGG_FIRST:
            return &aggFirst;
        case TS_AGG_LAST:
            return &aggLast;
        case TS_AGG_RANGE:
            return &aggRange;
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return NULL;
}
