/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "rdb.h"

#include "consts.h"
#include "endianconv.h"

#include <string.h>
#include <rmutil/alloc.h>

void *series_rdb_load(RedisModuleIO *io, int encver) {
    if (encver < TS_ENC_VER || encver > TS_LATEST_ENCVER) {
        RedisModule_LogIOError(io, "error", "data is not in the correct encoding");
        return NULL;
    }
    SampleValue lastValue;
    timestamp_t lastTimestamp;
    uint64_t totalSamples;
    RedisModuleString *srcKey = NULL;

    CreateCtx cCtx = { 0 };
    RedisModuleString *keyName = RedisModule_LoadString(io);
    cCtx.retentionTime = RedisModule_LoadUnsigned(io);
    cCtx.chunkSizeBytes = RedisModule_LoadUnsigned(io);
    if (encver < TS_SIZE_RDB_VER) {
        cCtx.chunkSizeBytes *= SAMPLE_SIZE;
    }

    if (encver >= TS_UNCOMPRESSED_VER) {
        cCtx.options = RedisModule_LoadUnsigned(io);
    } else {
        cCtx.options |= SERIES_OPT_UNCOMPRESSED;
    }

    if (encver >= TS_SIZE_RDB_VER) {
        lastTimestamp = RedisModule_LoadUnsigned(io);
        if ((cCtx.options & SERIES_OPT_BLOB) == SERIES_OPT_BLOB)
            VALUE_BLOB(&lastValue) = RedisModule_LoadBlob(io);
        else
            VALUE_DOUBLE(&lastValue) = RedisModule_LoadDouble(io);
        totalSamples = RedisModule_LoadUnsigned(io);
        uint64_t hasSrcKey = RedisModule_LoadUnsigned(io);
        if (hasSrcKey) {
            srcKey = RedisModule_LoadString(io);
        }
    }

    cCtx.labelsCount = RedisModule_LoadUnsigned(io);
    cCtx.labels = malloc(sizeof(Label) * cCtx.labelsCount);
    for (int i = 0; i < cCtx.labelsCount; i++) {
        cCtx.labels[i].key = RedisModule_LoadString(io);
        cCtx.labels[i].value = RedisModule_LoadString(io);
    }

    uint64_t rulesCount = RedisModule_LoadUnsigned(io);

    Series *series = NewSeries(keyName, &cCtx);

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

    if (encver < TS_SIZE_RDB_VER) {
        uint64_t samplesCount = RedisModule_LoadUnsigned(io);
        for (size_t sampleIndex = 0; sampleIndex < samplesCount; sampleIndex++) {
            timestamp_t ts = RedisModule_LoadUnsigned(io);
            // Before TS_SIZE_RDB_VER, blobs are not supported
            double val = RedisModule_LoadDouble(io);
            SampleValue sValue = { .d.value = val };
            int result = SeriesAddSample(series, ts, sValue);
            if (result != TSDB_OK) {
                RedisModule_LogIOError(io, "warning", "couldn't load sample: %ld %lf", ts, val);
            }
        }
    } else {
        Chunk_t *chunk = NULL;
        // Free the default allocated chunk given LoadFromRDB will allocate a proper sized chunk
        timestamp_t rax_key = htonu64(0);
        chunk = (Chunk_t *)RedisModule_DictGetC(series->chunks, &rax_key, sizeof(rax_key), NULL);
        if (chunk != NULL) {
            series->funcs->FreeChunk(chunk);
        }
        dictOperator(series->chunks, NULL, 0, DICT_OP_DEL);
        uint64_t numChunks = RedisModule_LoadUnsigned(io);
        for (int i = 0; i < numChunks; ++i) {
            series->funcs->LoadFromRDB(&chunk, io, SeriesIsBlob(series));
            dictOperator(
                series->chunks, chunk, series->funcs->GetFirstTimestamp(chunk), DICT_OP_SET);
        }
        series->totalSamples = totalSamples;
        series->srcKey = srcKey;
        series->lastTimestamp = lastTimestamp;
        series->lastValue = lastValue;
        series->lastChunk = chunk;
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
    RedisModule_SaveUnsigned(io, series->chunkSizeBytes);
    RedisModule_SaveUnsigned(io, series->options);
    RedisModule_SaveUnsigned(io, series->lastTimestamp);

    if (SeriesIsBlob(series)) {
        RedisModule_SaveBlob(io, VALUE_BLOB(&series->lastValue));
    } else
        RedisModule_SaveDouble(io, VALUE_DOUBLE(&series->lastValue));

    RedisModule_SaveUnsigned(io, series->totalSamples);
    if (series->srcKey != NULL) {
        RedisModule_SaveUnsigned(io, TRUE);
        RedisModule_SaveString(io, series->srcKey);
    } else {
        RedisModule_SaveUnsigned(io, FALSE);
    }

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

    Chunk_t *chunk;
    uint64_t numChunks = RedisModule_DictSize(series->chunks);
    RedisModule_SaveUnsigned(io, numChunks);
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    while (RedisModule_DictNextC(iter, NULL, &chunk)) {
        series->funcs->SaveToRDB(chunk, io, SeriesIsBlob(series));
        numChunks--;
    }
    RedisModule_DictIteratorStop(iter);
}
