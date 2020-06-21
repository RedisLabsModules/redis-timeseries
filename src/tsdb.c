/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "tsdb.h"

#include "config.h"
#include "consts.h"
#include "endianconv.h"
#include "indexer.h"
#include "module.h"

#include <redismodule.h>
#include <string.h>
#include <time.h>
#include <rmutil/alloc.h>
#include <rmutil/logging.h>
#include <rmutil/strings.h>

static Series *lastDeletedSeries = NULL;

Series *NewSeries(RedisModuleString *keyName,
                  Label *labels,
                  size_t labelsCount,
                  uint64_t retentionTime,
                  short maxSamplesPerChunk,
                  int uncompressed) {
    Series *newSeries = (Series *)malloc(sizeof(Series));
    newSeries->keyName = keyName;
    newSeries->chunks = RedisModule_CreateDict(NULL);
    newSeries->maxSamplesPerChunk = maxSamplesPerChunk;
    newSeries->retentionTime = retentionTime;
    newSeries->srcKey = NULL;
    newSeries->rules = NULL;
    newSeries->lastTimestamp = 0;
    newSeries->lastValue = 0;
    newSeries->totalSamples = 0;
    newSeries->labels = labels;
    newSeries->labelsCount = labelsCount;
    newSeries->options = 0;
    if (uncompressed & SERIES_OPT_UNCOMPRESSED) {
        newSeries->options |= SERIES_OPT_UNCOMPRESSED;
        newSeries->funcs = GetChunkClass(CHUNK_REGULAR);
    } else {
        newSeries->funcs = GetChunkClass(CHUNK_COMPRESSED);
    }
    newSeries->lastChunk = NULL;
    return newSeries;
}

void SeriesTrim(Series *series) {
    if (series->retentionTime == 0) {
        return;
    }

    // start iterator from smallest key
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    Chunk_t *currentChunk;
    void *currentKey;
    size_t keyLen;
    timestamp_t minTimestamp = series->lastTimestamp > series->retentionTime
                                   ? series->lastTimestamp - series->retentionTime
                                   : 0;

    while ((currentKey = RedisModule_DictNextC(iter, &keyLen, (void *)&currentChunk))) {
        if (series->funcs->GetLastTimestamp(currentChunk) < minTimestamp) {
            RedisModule_DictDelC(series->chunks, currentKey, keyLen, NULL);
            // reseek iterator since we modified the dict, go to first element that is bigger than
            // current key
            RedisModule_DictIteratorReseekC(iter, ">", currentKey, keyLen);

            series->totalSamples -= series->funcs->GetNumOfSample(currentChunk);
            series->funcs->FreeChunk(currentChunk);
        } else {
            break;
        }
    }
    RedisModule_DictIteratorStop(iter);
}

// Encode timestamps as bigendian to allow correct lexical sorting
static void seriesEncodeTimestamp(void *buf, timestamp_t timestamp) {
    uint64_t e;
    e = htonu64(timestamp);
    memcpy(buf, &e, sizeof(e));
}

void freeLastDeletedSeries() {
    if (lastDeletedSeries == NULL) {
        return;
    }
    CompactionRule *rule = lastDeletedSeries->rules;
    while (rule != NULL) {
        CompactionRule *nextRule = rule->nextRule;
        FreeCompactionRule(rule);
        rule = nextRule;
    }
    if (lastDeletedSeries->srcKey != NULL) {
        RedisModule_FreeString(NULL, lastDeletedSeries->srcKey);
    }
    RedisModule_FreeString(NULL, lastDeletedSeries->keyName);
    free(lastDeletedSeries);
    lastDeletedSeries = NULL;
}

void CleanLastDeletedSeries(RedisModuleCtx *ctx, RedisModuleString *key) {
    if (lastDeletedSeries != NULL &&
        RedisModule_StringCompare(lastDeletedSeries->keyName, key) == 0) {
        CompactionRule *rule = lastDeletedSeries->rules;
        while (rule != NULL) {
            RedisModuleKey *seriesKey;
            Series *dstSeries;
            const int status = GetSeries(
                ctx, rule->destKey, &seriesKey, &dstSeries, REDISMODULE_READ | REDISMODULE_WRITE);
            if (status) {
                SeriesDeleteSrcRule(dstSeries, lastDeletedSeries->keyName);
                RedisModule_CloseKey(seriesKey);
            }
            rule = rule->nextRule;
        }
        if (lastDeletedSeries->srcKey) {
            RedisModuleKey *seriesKey;
            Series *srcSeries;
            const int status = GetSeries(ctx,
                                         lastDeletedSeries->srcKey,
                                         &seriesKey,
                                         &srcSeries,
                                         REDISMODULE_READ | REDISMODULE_WRITE);
            if (status) {
                SeriesDeleteRule(srcSeries, lastDeletedSeries->keyName);
                RedisModule_CloseKey(seriesKey);
            }
        }
    }
    freeLastDeletedSeries();
}

// Releases Series and all its compaction rules
void FreeSeries(void *value) {
    Series *currentSeries = (Series *)value;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(currentSeries->chunks, "^", NULL, 0);
    Chunk_t *currentChunk;
    while (RedisModule_DictNextC(iter, NULL, (void *)&currentChunk) != NULL) {
        currentSeries->funcs->FreeChunk(currentChunk);
    }
    RedisModule_DictIteratorStop(iter);

    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModule_AutoMemory(ctx);
    RemoveIndexedMetric(
        ctx, currentSeries->keyName, currentSeries->labels, currentSeries->labelsCount);

    FreeLabels(currentSeries->labels, currentSeries->labelsCount);

    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_FreeDict(NULL, currentSeries->chunks);

    freeLastDeletedSeries();
    lastDeletedSeries = currentSeries;
}

void FreeCompactionRule(void *value) {
    CompactionRule *rule = (CompactionRule *)value;
    RedisModule_FreeString(NULL, rule->destKey);
    ((AggregationClass *)rule->aggClass)->freeContext(rule->aggContext);
    free(rule);
}

size_t SeriesGetChunksSize(Series *series) {
    size_t size = 0;
    Chunk_t *currentChunk;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    while (RedisModule_DictNextC(iter, NULL, (void *)&currentChunk)) {
        size += series->funcs->GetChunkSize(currentChunk);
    }
    RedisModule_DictIteratorStop(iter);
    return size;
}

size_t SeriesMemUsage(const void *value) {
    Series *series = (Series *)value;

    size_t labelLen = 0;
    uint32_t labelsLen = 0;
    for (int i = 0; i < series->labelsCount; i++) {
        RedisModule_StringPtrLen(series->labels[i].key, &labelLen);
        labelsLen += (labelLen + 1);
        RedisModule_StringPtrLen(series->labels[i].value, &labelLen);
        labelsLen += (labelLen + 1);
    }

    size_t rulesSize = 0;
    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        rulesSize += sizeof(CompactionRule);
        rule = rule->nextRule;
    }

    return sizeof(series) + rulesSize + labelsLen + sizeof(Label) * series->labelsCount +
           SeriesGetChunksSize(series);
}

size_t SeriesGetNumSamples(const Series *series) {
    size_t numSamples = 0;
    if (series != NULL) {
        numSamples = series->totalSamples;
    }
    return numSamples;
}

static Chunk_t *addChunk(Series *series, api_timestamp_t timestamp) {
    timestamp_t rax_key;
    Chunk_t *newChunk = series->funcs->NewChunk(series->maxSamplesPerChunk);
    seriesEncodeTimestamp(&rax_key, timestamp);
    RedisModule_DictSetC(series->chunks, &rax_key, sizeof(rax_key), (void *)newChunk);
    series->lastChunk = newChunk;
    return newChunk;
}

int SeriesAddSample(Series *series, api_timestamp_t timestamp, double value) {
    if (series->lastChunk == NULL) {
        addChunk(series, timestamp);
    }
    if (timestamp <= series->lastTimestamp && series->lastTimestamp != 0) {
        return TSDB_ERR_TIMESTAMP_TOO_OLD;
    } else if (timestamp == series->lastTimestamp && timestamp != 0) {
        return TSDB_ERR_TIMESTAMP_OCCUPIED;
    }
    Sample sample = { .timestamp = timestamp, .value = value };
    int ret = series->funcs->AddSample(series->lastChunk, &sample);
    if (ret == CR_END) {
        // When a new chunk is created trim the series
        SeriesTrim(series);

        Chunk_t *newChunk = addChunk(series, timestamp);
        series->funcs->AddSample(newChunk, &sample);
    }
    series->lastTimestamp = timestamp;
    series->lastValue = value;
    series->totalSamples++;
    return TSDB_OK;
}

// Initiates SeriesIterator, find the correct chunk and initiate a ChunkIterator
SeriesIterator SeriesQuery(Series *series, timestamp_t start_ts, timestamp_t end_ts, bool rev) {
    SeriesIterator iter = { 0 };
    iter.series = series;
    iter.minTimestamp = start_ts;
    iter.maxTimestamp = end_ts;
    iter.reverse = rev;

    timestamp_t rax_key;
    ChunkFuncs *funcs = series->funcs;

    if (iter.reverse == false) {
        iter.GetNext = funcs->ChunkIteratorGetNext;
        iter.DictGetNext = RedisModule_DictNextC;
        seriesEncodeTimestamp(&rax_key, iter.minTimestamp);
    } else {
        iter.GetNext = funcs->ChunkIteratorGetPrev;
        iter.DictGetNext = RedisModule_DictPrevC;
        seriesEncodeTimestamp(&rax_key, iter.maxTimestamp);
    }

    // get first chunk within query range
    iter.dictIter = RedisModule_DictIteratorStartC(series->chunks, "<=", &rax_key, sizeof(rax_key));
    if (!iter.DictGetNext(iter.dictIter, NULL, (void *)&iter.currentChunk)) {
        RedisModule_DictIteratorReseekC(iter.dictIter, "^", NULL, 0);
        if (!iter.DictGetNext(iter.dictIter, NULL, (void *)&iter.currentChunk)) {
            return (SeriesIterator){ 0 };
        }
    }

    iter.chunkIterator = funcs->NewChunkIterator(iter.currentChunk, iter.reverse);
    return iter;
}

void SeriesIteratorClose(SeriesIterator *iterator) {
    iterator->series->funcs->FreeChunkIterator(iterator->chunkIterator, iterator->reverse);
    RedisModule_DictIteratorStop(iterator->dictIter);
}

// Fills sample from chunk. If all samples were extracted from the chunk, we
// move to the next chunk.
ChunkResult SeriesIteratorGetNext(SeriesIterator *iterator, Sample *currentSample) {
    ChunkResult res;
    ChunkFuncs *funcs = iterator->series->funcs;
    Chunk_t *currentChunk = iterator->currentChunk;

    while (true) {
        res = iterator->GetNext(iterator->chunkIterator, currentSample);
        if (res == CR_END) { // Reached the end of the chunk
            if (!iterator->DictGetNext(iterator->dictIter, NULL, (void *)&currentChunk) ||
                funcs->GetFirstTimestamp(currentChunk) > iterator->maxTimestamp ||
                funcs->GetLastTimestamp(currentChunk) < iterator->minTimestamp) {
                return CR_END; // No more chunks or they out of range
            }
            funcs->FreeChunkIterator(iterator->chunkIterator, iterator->reverse);
            iterator->chunkIterator = funcs->NewChunkIterator(currentChunk, iterator->reverse);
            if (iterator->GetNext(iterator->chunkIterator, currentSample) != CR_OK) {
                return CR_END;
            }
        }

        // check timestamp is within range
        if (!iterator->reverse) {
            // forward range handling
            if (currentSample->timestamp < iterator->minTimestamp) {
                // didn't reach the starting point of the requested range
                continue;
            }
            if (currentSample->timestamp > iterator->maxTimestamp) {
                // reached the end of the requested range
                return CR_END;
            }
        } else {
            // reverse range handling
            if (currentSample->timestamp > iterator->maxTimestamp) {
                // didn't reach our starting range
                continue;
            }
            if (currentSample->timestamp < iterator->minTimestamp) {
                // didn't reach the starting point of the requested range
                return CR_END;
            }
        }
        return CR_OK;
    }
    return CR_OK;
}

CompactionRule *SeriesAddRule(Series *series,
                              RedisModuleString *destKeyStr,
                              int aggType,
                              uint64_t timeBucket) {
    CompactionRule *rule = NewRule(destKeyStr, aggType, timeBucket);
    if (rule == NULL) {
        return NULL;
    }
    if (series->rules == NULL) {
        series->rules = rule;
    } else {
        CompactionRule *last = series->rules;
        while (last->nextRule != NULL)
            last = last->nextRule;
        last->nextRule = rule;
    }
    return rule;
}

int SeriesCreateRulesFromGlobalConfig(RedisModuleCtx *ctx,
                                      RedisModuleString *keyName,
                                      Series *series,
                                      Label *labels,
                                      size_t labelsCount) {
    size_t len;
    int i;
    Series *compactedSeries;
    RedisModuleKey *compactedKey;
    size_t compactedRuleLabelCount = labelsCount + 2;

    for (i = 0; i < TSGlobalConfig.compactionRulesCount; i++) {
        SimpleCompactionRule *rule = TSGlobalConfig.compactionRules + i;
        const char *aggString = AggTypeEnumToString(rule->aggType);
        RedisModuleString *destKey = RedisModule_CreateStringPrintf(
            ctx, "%s_%s_%ld", RedisModule_StringPtrLen(keyName, &len), aggString, rule->timeBucket);
        RedisModule_RetainString(ctx, destKey);
        compactedKey = RedisModule_OpenKey(ctx, destKey, REDISMODULE_READ | REDISMODULE_WRITE);
        if (RedisModule_KeyType(compactedKey) != REDISMODULE_KEYTYPE_EMPTY) {
            // TODO: should we break here? Is log enough?
            RM_LOG_WARNING(ctx, "Cannot create compacted key, key '%s' already exists", destKey);
            RedisModule_CloseKey(compactedKey);
            continue;
        }
        SeriesAddRule(series, destKey, rule->aggType, rule->timeBucket);

        Label *compactedLabels = malloc(sizeof(Label) * compactedRuleLabelCount);
        // todo: deep copy labels function
        for (int l = 0; l < labelsCount; l++) {
            compactedLabels[l].key = RedisModule_CreateStringFromString(NULL, labels[l].key);
            compactedLabels[l].value = RedisModule_CreateStringFromString(NULL, labels[l].value);
        }

        // For every aggregated key create 2 labels: `aggregation` and `time_bucket`.
        compactedLabels[labelsCount].key = RedisModule_CreateStringPrintf(NULL, "aggregation");
        compactedLabels[labelsCount].value =
            RedisModule_CreateString(NULL, aggString, strlen(aggString));
        compactedLabels[labelsCount + 1].key = RedisModule_CreateStringPrintf(NULL, "time_bucket");
        compactedLabels[labelsCount + 1].value =
            RedisModule_CreateStringPrintf(NULL, "%ld", rule->timeBucket);

        CreateTsKey(ctx,
                    destKey,
                    compactedLabels,
                    compactedRuleLabelCount,
                    rule->retentionSizeMillisec,
                    TSGlobalConfig.maxSamplesPerChunk,
                    TSGlobalConfig.options & SERIES_OPT_UNCOMPRESSED,
                    &compactedSeries,
                    &compactedKey);
        RedisModule_CloseKey(compactedKey);
    }
    return TSDB_OK;
}

CompactionRule *NewRule(RedisModuleString *destKey, int aggType, uint64_t timeBucket) {
    if (timeBucket == 0ULL) {
        return NULL;
    }

    CompactionRule *rule = (CompactionRule *)malloc(sizeof(CompactionRule));
    rule->aggClass = GetAggClass(aggType);
    ;
    rule->aggType = aggType;
    rule->aggContext = rule->aggClass->createContext();
    rule->timeBucket = timeBucket;
    rule->destKey = destKey;
    rule->startCurrentTimeBucket = -1LL;

    rule->nextRule = NULL;

    return rule;
}

int SeriesDeleteRule(Series *series, RedisModuleString *destKey) {
    CompactionRule *rule = series->rules;
    CompactionRule *prev_rule = NULL;
    while (rule != NULL) {
        if (RMUtil_StringEquals(rule->destKey, destKey)) {
            CompactionRule *next = rule->nextRule;
            FreeCompactionRule(rule);
            if (prev_rule != NULL) {
                // cut off the current rule from the linked list
                prev_rule->nextRule = next;
            } else {
                // make the next one to be the first rule
                series->rules = next;
            }
            return TRUE;
        }
        prev_rule = rule;
        rule = rule->nextRule;
    }
    return FALSE;
}

int SeriesSetSrcRule(Series *series, RedisModuleString *srctKey) {
    if (series->srcKey) {
        return FALSE;
    }
    series->srcKey = srctKey;
    return TRUE;
}

int SeriesDeleteSrcRule(Series *series, RedisModuleString *srctKey) {
    if (RMUtil_StringEquals(series->srcKey, srctKey)) {
        RedisModule_FreeString(NULL, series->srcKey);
        series->srcKey = NULL;
        return TRUE;
    }
    return FALSE;
}
