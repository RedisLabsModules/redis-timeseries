/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "compaction.h"

#include "blob.h"
#include "generic_chunk.h"

#include <ctype.h>
#include <math.h> // sqrt
#include <string.h>
#include <rmutil/alloc.h>

typedef struct MaxMinContext
{
    double minValue;
    double maxValue;
    char isResetted;
} MaxMinContext;

typedef struct SingleValueContext
{
    double value;
    char isResetted;
} SingleValueContext;

typedef struct BlobValueContext
{
    TSBlob *blob;
    double count;
    char isResetted;
} BlobValueContext;

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

/* Blobs */

static void BlobValueReset(void *contextPtr) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;

    if (context->blob) {
        FreeBlob(context->blob);
        context->blob = NULL;
    }
    context->count = 0;
    context->isResetted = TRUE;
}

static void BlobFirstReset(void *contextPtr) {
    BlobValueReset(contextPtr);
}

static void BlobLastReset(void *contextPtr) {
    BlobValueReset(contextPtr);
}

static void *BlobValueCreateContext() {
    BlobValueContext *context = (BlobValueContext *)RedisModule_Alloc(sizeof(BlobValueContext));

    context->blob = NULL;
    BlobValueReset(context);
    return context;
}

static void BlobCountAppendValue(void *contextPtr, double val) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    (void)val;

    context->count++;
    context->isResetted = FALSE;
}

static int BlobCountValueFinalize(void *contextPtr, double *val) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;

    if (context->isResetted)
        return TSDB_ERROR;

    *val = context->count;
    return TSDB_OK;
}

static void BlobCountWriteContext(void *contextPtr, RedisModuleIO *io) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    RedisModule_SaveDouble(io, context->count);
}

static void BlobCountReadContext(void *contextPtr, RedisModuleIO *io) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    context->count = RedisModule_LoadDouble(io);
}

static void BlobFirstAppendValue(void *contextPtr, double value) {
    SampleValue val;
    VALUE_DOUBLE(&val) = value;

    BlobValueContext *context = (BlobValueContext *)contextPtr;

    if (!context->isResetted)
        return;

    context->isResetted = false;
    TSBlob *srcblob = VALUE_BLOB(&val);

    if (context->blob)
        FreeBlob(context->blob);

    context->blob = BlobDup(srcblob);
}

static void BlobLastAppendValue(void *contextPtr, double value) {
    SampleValue val;
    VALUE_DOUBLE(&val) = value;

    BlobValueContext *context = (BlobValueContext *)contextPtr;

    TSBlob *srcblob = VALUE_BLOB(&val);
    if (context->blob)
        FreeBlob(context->blob);

    context->blob = BlobDup(srcblob);
    context->isResetted = false;
}

static int BlobValueFinalize(void *contextPtr, double *val) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;

    if (context->isResetted)
        return TSDB_ERROR;

    TSBlob **blob = (TSBlob **)val;

    BlobCopy(*blob, context->blob);
    return TSDB_OK;
}

static int BlobFirstFinalize(void *contextPtr, double *val) {
    return BlobValueFinalize(contextPtr, val);
}

static int BlobLastFinalize(void *contextPtr, double *val) {
    return BlobValueFinalize(contextPtr, val);
}

static void BlobValueDeleteContext(void *contextPtr) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;

    if (context->blob) {
        FreeBlob(context->blob);
    }

    RedisModule_Free(context);
}

static void BlobCountDeleteContext(void *contextPtr) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    RedisModule_Free(context);
}

static void BlobFirstDeleteContext(void *contextPtr) {
    BlobValueDeleteContext(contextPtr);
}

static void BlobLastDeleteContext(void *contextPtr) {
    BlobValueDeleteContext(contextPtr);
}

static void BlobValueWriteContext(void *contextPtr, RedisModuleIO *io) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    RedisModule_SaveBlob(io, context->blob);
}

static void BlobValueReadContext(void *contextPtr, RedisModuleIO *io) {
    BlobValueContext *context = (BlobValueContext *)contextPtr;
    context->blob = RedisModule_LoadBlob(io);
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

void *MaxMinCreateContext() {
    MaxMinContext *context = (MaxMinContext *)malloc(sizeof(MaxMinContext));
    context->minValue = 0;
    context->maxValue = 0;
    context->isResetted = TRUE;
    return context;
}

void MaxMinAppendValue(void *contextPtr, double value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
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

int MaxFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->maxValue;
    return TSDB_OK;
}

int MinFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->minValue;
    return TSDB_OK;
}

int RangeFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    if (context->isResetted == TRUE) {
        return TSDB_ERROR;
    }
    *value = context->maxValue - context->minValue;
    return TSDB_OK;
}

void MaxMinReset(void *contextPtr) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    context->maxValue = 0;
    context->minValue = 0;
    context->isResetted = TRUE;
}

void MaxMinWriteContext(void *contextPtr, RedisModuleIO *io) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    RedisModule_SaveDouble(io, context->maxValue);
    RedisModule_SaveDouble(io, context->minValue);
    RedisModule_SaveStringBuffer(io, &context->isResetted, 1);
}

void MaxMinReadContext(void *contextPtr, RedisModuleIO *io) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    size_t len = 1;
    context->maxValue = RedisModule_LoadDouble(io);
    context->minValue = RedisModule_LoadDouble(io);
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

static AggregationClass aggMax = { .createContext = MaxMinCreateContext,
                                   .appendValue = MaxMinAppendValue,
                                   .freeContext = rm_free,
                                   .finalize = MaxFinalize,
                                   .writeContext = MaxMinWriteContext,
                                   .readContext = MaxMinReadContext,
                                   .resetContext = MaxMinReset };

static AggregationClass aggMin = { .createContext = MaxMinCreateContext,
                                   .appendValue = MaxMinAppendValue,
                                   .freeContext = rm_free,
                                   .finalize = MinFinalize,
                                   .writeContext = MaxMinWriteContext,
                                   .readContext = MaxMinReadContext,
                                   .resetContext = MaxMinReset };

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

static AggregationClass blobAggCount = { .createContext = BlobValueCreateContext,
                                         .appendValue = BlobCountAppendValue,
                                         .freeContext = BlobCountDeleteContext,
                                         .finalize = BlobCountValueFinalize,
                                         .writeContext = BlobCountWriteContext,
                                         .readContext = BlobCountReadContext,
                                         .resetContext = BlobValueReset };

static AggregationClass blobAggFirst = { .createContext = BlobValueCreateContext,
                                         .appendValue = BlobFirstAppendValue,
                                         .freeContext = BlobFirstDeleteContext,
                                         .finalize = BlobFirstFinalize,
                                         .writeContext = BlobValueWriteContext,
                                         .readContext = BlobValueReadContext,
                                         .resetContext = BlobFirstReset };

static AggregationClass blobAggLast = { .createContext = BlobValueCreateContext,
                                        .appendValue = BlobLastAppendValue,
                                        .freeContext = BlobLastDeleteContext,
                                        .finalize = BlobLastFinalize,
                                        .writeContext = BlobValueWriteContext,
                                        .readContext = BlobValueReadContext,
                                        .resetContext = BlobLastReset };

static AggregationClass aggRange = { .createContext = MaxMinCreateContext,
                                     .appendValue = MaxMinAppendValue,
                                     .freeContext = rm_free,
                                     .finalize = RangeFinalize,
                                     .writeContext = MaxMinWriteContext,
                                     .readContext = MaxMinReadContext,
                                     .resetContext = MaxMinReset };

AggregationClass *BlobAggClass(AggregationClass *class) {
    AggregationClass *blobClass = class;
    if (class == &aggCount)
        blobClass = &blobAggCount;
    else if (class == &aggFirst)
        blobClass = &blobAggFirst;
    else if (class == &aggLast)
        blobClass = &blobAggLast;

    return blobClass;
}

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
        case TS_AGG_BLOB_COUNT:
            return "COUNT";
        case TS_AGG_FIRST:
        case TS_AGG_BLOB_FIRST:
            return "FIRST";
        case TS_AGG_LAST:
        case TS_AGG_BLOB_LAST:
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
        case TS_AGG_BLOB_COUNT:
            return &blobAggCount;
        case TS_AGG_BLOB_FIRST:
            return &blobAggFirst;
        case TS_AGG_BLOB_LAST:
            return &blobAggLast;
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return NULL;
}

// the only supported aggregation types for blob data

bool IsCompactionBlobFriendly(TS_AGG_TYPES_T aggType) {
    return (aggType == TS_AGG_NONE || aggType == TS_AGG_COUNT || aggType == TS_AGG_FIRST ||
            aggType == TS_AGG_LAST);
}

// returns true if the aggregated result is of type blob

bool aggClassIsBlob(const AggregationClass *class) {
    return (class == &blobAggFirst || class == &blobAggLast);
}

TS_AGG_TYPES_T BlobAggType(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_COUNT:
            return TS_AGG_BLOB_COUNT;
        case TS_AGG_FIRST:
            return TS_AGG_BLOB_FIRST;
        case TS_AGG_LAST:
            return TS_AGG_BLOB_LAST;
        default:
            break;
    }
    return aggType;
}