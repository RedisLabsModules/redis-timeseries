#include "rdb.h"
#include "chunk.h"

void *series_rdb_load(RedisModuleIO *io, int encver)
{
    if (encver != TS_ENC_VER) {
        RedisModule_LogIOError(io, "error", "data is not in the correct encoding");
        return NULL;
    }
    uint64_t retentionSecs = RedisModule_LoadUnsigned(io);
    uint64_t maxSamplesPerChunk = RedisModule_LoadUnsigned(io);
    uint64_t rulesCount = RedisModule_LoadUnsigned(io);
    
    Series *series = NewSeries(retentionSecs, maxSamplesPerChunk);

    CompactionRule *lastRule;
    RedisModuleCtx *ctx = RedisModule_GetContextFromIO(io);

    for (int i = 0; i < rulesCount; i++) {
        RedisModuleString *destKey = RedisModule_LoadString(io);
        uint64_t bucketSizeSec = RedisModule_LoadUnsigned(io);
        uint64_t aggType = RedisModule_LoadUnsigned(io);

        destKey = RedisModule_CreateStringFromString(ctx, destKey);
        RedisModule_RetainString(ctx, destKey);

        CompactionRule *rule = NewRule(destKey, aggType, bucketSizeSec);
        
        if (series->rules == NULL) {
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
        SeriesAddSample(series, ts, val);
    }
    return series;
}

int countRules(Series *series) {
    int count = 0;
    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        count ++;
        rule = rule->nextRule;
    }
    return count;
}

void series_rdb_save(RedisModuleIO *io, void *value)
{
    Series *series = value;
    RedisModule_SaveUnsigned(io, series->retentionSecs);
    RedisModule_SaveUnsigned(io, series->maxSamplesPerChunk);
    RedisModule_SaveUnsigned(io, countRules(series));

    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        RedisModule_SaveString(io, rule->destKey);
        RedisModule_SaveUnsigned(io, rule->bucketSizeSec);
        RedisModule_SaveUnsigned(io, rule->aggType);
        rule->aggClass->writeContext(rule->aggContext, io);
        rule = rule->nextRule;
    }

    size_t numSamples = SeriesGetNumSamples(series);
    RedisModule_SaveUnsigned(io, numSamples);

    SeriesIterator iter = SeriesQuery(series, 0, series->lastTimestamp);
    Sample sample;
    while (SeriesIteratorGetNext(&iter, &sample) != 0) {
        RedisModule_SaveUnsigned(io, sample.timestamp);
        RedisModule_SaveDouble(io, sample.data);
    }
}