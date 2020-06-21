/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "rdb.h"

#include "consts.h"

#include <string.h>
#include <rmutil/alloc.h>

void *series_rdb_load(RedisModuleIO *io, int encver) {
    if (encver != TS_ENC_VER && encver != TS_UNCOMPRESSED_VER) {
        RedisModule_LogIOError(io, "error", "data is not in the correct encoding");
        return NULL;
    }
    RedisModuleString *keyName = RedisModule_LoadString(io);
    uint64_t retentionTime = RedisModule_LoadUnsigned(io);
    uint64_t maxSamplesPerChunk = RedisModule_LoadUnsigned(io);

    short options = 0;
    if (encver >= TS_UNCOMPRESSED_VER) {
        options = RedisModule_LoadUnsigned(io);
    } else {
        options |= SERIES_OPT_UNCOMPRESSED;
    }

    uint64_t labelsCount = RedisModule_LoadUnsigned(io);
    Label *labels = malloc(sizeof(Label) * labelsCount);
    for (int i = 0; i < labelsCount; i++) {
        labels[i].key = RedisModule_LoadString(io);
        labels[i].value = RedisModule_LoadString(io);
    }

    uint64_t rulesCount = RedisModule_LoadUnsigned(io);

    Series *series = NewSeries(keyName,
                               labels,
                               labelsCount,
                               retentionTime,
                               maxSamplesPerChunk,
                               options & SERIES_OPT_UNCOMPRESSED);

    CompactionRule *lastRule = NULL;
    RedisModuleCtx *ctx = RedisModule_GetContextFromIO(io);

    for (int i = 0; i < rulesCount; i++) {
        RedisModuleString *destKey = RedisModule_LoadString(io);
        uint64_t timeBucket = RedisModule_LoadUnsigned(io);
        uint64_t aggType = RedisModule_LoadUnsigned(io);

        CompactionRule *rule = NewRule(destKey, aggType, timeBucket);
        rule->startCurrentTimeBucket = RedisModule_LoadUnsigned(io);

        if (i == 0) {
            series->rules = rule;
        } else {
            lastRule->nextRule = rule;
        }
        rule->aggClass->readContext(rule->aggContext, io);
        lastRule = rule;
    }

    uint64_t samplesCount = RedisModule_LoadUnsigned(io);
    for (size_t sampleIndex = 0; sampleIndex < samplesCount; sampleIndex++) {
        timestamp_t ts = RedisModule_LoadUnsigned(io);
        double val = RedisModule_LoadDouble(io);
        int result = SeriesAddSample(series, ts, val);
        if (result != TSDB_OK) {
            RedisModule_LogIOError(io, "warning", "couldn't load sample: %ld %lf", ts, val);
        }
    }

    IndexMetric(ctx, keyName, series->labels, series->labelsCount);
    return series;
}

unsigned int countRules(Series *series) {
    unsigned int count = 0;
    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        count++;
        rule = rule->nextRule;
    }
    return count;
}

void series_rdb_save(RedisModuleIO *io, void *value) {
    Series *series = value;
    RedisModule_SaveString(io, series->keyName);
    RedisModule_SaveUnsigned(io, series->retentionTime);
    RedisModule_SaveUnsigned(io, series->maxSamplesPerChunk);
    RedisModule_SaveUnsigned(io, series->options);

    RedisModule_SaveUnsigned(io, series->labelsCount);
    for (int i = 0; i < series->labelsCount; i++) {
        RedisModule_SaveString(io, series->labels[i].key);
        RedisModule_SaveString(io, series->labels[i].value);
    }

    RedisModule_SaveUnsigned(io, countRules(series));

    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        RedisModule_SaveString(io, rule->destKey);
        RedisModule_SaveUnsigned(io, rule->timeBucket);
        RedisModule_SaveUnsigned(io, rule->aggType);
        RedisModule_SaveUnsigned(io, rule->startCurrentTimeBucket);
        rule->aggClass->writeContext(rule->aggContext, io);
        rule = rule->nextRule;
    }

    size_t numSamples = SeriesGetNumSamples(series);
    RedisModule_SaveUnsigned(io, numSamples);

    SeriesIterator iter = SeriesQuery(series, 0, series->lastTimestamp, false);
    if (iter.series == 0) {
        return;
    }
    Sample sample;
    while (SeriesIteratorGetNext(&iter, &sample) == CR_OK) {
        RedisModule_SaveUnsigned(io, sample.timestamp);
        RedisModule_SaveDouble(io, sample.value);
    }
    SeriesIteratorClose(&iter);
}
