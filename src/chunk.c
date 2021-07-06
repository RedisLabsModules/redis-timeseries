/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "chunk.h"

#include "gears_integration.h"

#include "rmutil/alloc.h"

Chunk_t *Uncompressed_NewChunk(bool isBlob, size_t size) {
    Chunk *newChunk = (Chunk *)malloc(sizeof(Chunk));
    newChunk->num_samples = 0;
    newChunk->size = size;
    newChunk->samples = (Sample *)malloc(size);
#ifdef DEBUG
    memset(newChunk->samples, 0, size);
#endif
    newChunk->isBlob = isBlob;
    newChunk->base_timestamp = 0;

    // pre-allocate blob containers
    if (isBlob) {
        for (Sample *sample = newChunk->samples; (char *)sample < (char *)newChunk->samples + size;
             sample++) {
            VALUE_BLOB(&sample->value) = NewBlob(NULL, 0);
        }
    }

    return newChunk;
}

void Uncompressed_FreeChunk(Chunk_t *chunk) {
    Chunk *thisChunk = (Chunk *)chunk;
    if (thisChunk->isBlob) {
        for (Sample *sample = thisChunk->samples;
             (char *)sample < (char *)thisChunk->samples + thisChunk->size;
             sample++) {
            FreeBlob(VALUE_BLOB(&sample->value));
        }
    }

    free(thisChunk->samples);
    free(thisChunk);
}

/**
 * TODO: describe me
 * @param chunk
 * @return
 */
Chunk_t *Uncompressed_SplitChunk(Chunk_t *chunk) {
    Chunk *curChunk = (Chunk *)chunk;
    size_t split = curChunk->num_samples / 2;
    size_t curNumSamples = curChunk->num_samples - split;

    // create chunk and copy samples
    Chunk *newChunk = Uncompressed_NewChunk(curChunk->isBlob, split * SAMPLE_SIZE);
    for (size_t i = 0; i < split; ++i) {
        Sample *sample = &curChunk->samples[curNumSamples + i];
        Uncompressed_AddSample(newChunk, sample);
    }

    // update current chunk
    curChunk->num_samples = curNumSamples;
    curChunk->size = curNumSamples * SAMPLE_SIZE;
    curChunk->samples = realloc(curChunk->samples, curChunk->size);

    return newChunk;
}

static int IsChunkFull(Chunk *chunk) {
    return chunk->num_samples == chunk->size / SAMPLE_SIZE;
}

u_int64_t Uncompressed_NumOfSample(Chunk_t *chunk) {
    return ((Chunk *)chunk)->num_samples;
}

static Sample *ChunkGetSample(Chunk *chunk, int index) {
    return &chunk->samples[index];
}

timestamp_t Uncompressed_GetLastTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->num_samples == 0) {
        return -1;
    }
    return ChunkGetSample(chunk, ((Chunk *)chunk)->num_samples - 1)->timestamp;
}

timestamp_t Uncompressed_GetFirstTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->num_samples == 0) {
        return -1;
    }
    return ChunkGetSample(chunk, 0)->timestamp;
}

ChunkResult Uncompressed_AddSample(Chunk_t *chunk, Sample *sample) {
    Chunk *regChunk = (Chunk *)chunk;
    if (IsChunkFull(regChunk)) {
        return CR_END;
    }

    if (Uncompressed_NumOfSample(regChunk) == 0) {
        // initialize base_timestamp
        regChunk->base_timestamp = sample->timestamp;
    }

    Sample *dstSample = &regChunk->samples[regChunk->num_samples];
    updateSampleValue(regChunk->isBlob, &dstSample->value, &sample->value);
    dstSample->timestamp = sample->timestamp;

    regChunk->num_samples++;

    return CR_OK;
}

/**
 * TODO: describe me
 * @param chunk
 * @param idx
 * @param sample
 */
static void upsertChunk(Chunk *chunk, size_t idx, Sample *sample) {
    if (chunk->num_samples == chunk->size / SAMPLE_SIZE) {
        chunk->size += sizeof(Sample);
        chunk->samples = realloc(chunk->samples, chunk->size);
    }
    if (idx < chunk->num_samples) { // sample is not last
        memmove(&chunk->samples[idx + 1],
                &chunk->samples[idx],
                (chunk->num_samples - idx) * sizeof(Sample));
    }
    chunk->samples[idx] = *sample;
    chunk->num_samples++;
}

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult Uncompressed_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy) {
    *size = 0;
    Chunk *regChunk = (Chunk *)uCtx->inChunk;
    timestamp_t ts = uCtx->sample.timestamp;
    short numSamples = regChunk->num_samples;
    // find sample location
    size_t i = 0;
    Sample *sample = NULL;
    for (; i < numSamples; ++i) {
        sample = ChunkGetSample(regChunk, i);
        if (ts <= sample->timestamp) {
            break;
        }
    }
    // update value in case timestamp exists
    if (sample != NULL && ts == sample->timestamp) {
        ChunkResult cr = handleDuplicateSample(duplicatePolicy, *sample, &uCtx->sample);
        if (cr != CR_OK) {
            return CR_ERR;
        }
        updateSampleValue(uCtx->isBlob, &regChunk->samples[i].value, &uCtx->sample.value);
        return CR_OK;
    }

    if (i == 0) {
        regChunk->base_timestamp = ts;
    }

    upsertChunk(regChunk, i, &uCtx->sample);
    *size = 1;
    return CR_OK;
}

size_t Uncompressed_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs) {
    Chunk *regChunk = (Chunk *)chunk;
    Sample *newSamples = (Sample *)malloc(regChunk->size);
    size_t i = 0;
    size_t new_count = 0;
    for (; i < regChunk->num_samples; ++i) {
        if (regChunk->samples[i].timestamp >= startTs && regChunk->samples[i].timestamp <= endTs) {
            continue;
        }
        newSamples[new_count++] = regChunk->samples[i];
    }
    size_t deleted_count = regChunk->num_samples - new_count;
    free(regChunk->samples);
    regChunk->samples = newSamples;
    regChunk->num_samples = new_count;
    regChunk->base_timestamp = newSamples[0].timestamp;
    return deleted_count;
}

ChunkIter_t *Uncompressed_NewChunkIterator(Chunk_t *chunk,
                                           int options,
                                           ChunkIterFuncs *retChunkIterClass) {
    ChunkIterator *iter = (ChunkIterator *)calloc(1, sizeof(ChunkIterator));
    iter->chunk = chunk;
    iter->options = options;
    if (options & CHUNK_ITER_OP_REVERSE) { // iterate from last to first
        iter->currentIndex = iter->chunk->num_samples - 1;
    } else { // iterate from first to last
        iter->currentIndex = 0;
    }

    if (retChunkIterClass != NULL) {
        *retChunkIterClass = *GetChunkIteratorClass(CHUNK_REGULAR);
    }

    return (ChunkIter_t *)iter;
}

ChunkResult Uncompressed_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->currentIndex < iter->chunk->num_samples) {
        *sample = *ChunkGetSample(iter->chunk, iter->currentIndex);
        iter->currentIndex++;
        return CR_OK;
    } else {
        return CR_END;
    }
}

ChunkResult Uncompressed_ChunkIteratorGetPrev(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->currentIndex >= 0) {
        *sample = *ChunkGetSample(iter->chunk, iter->currentIndex);
        iter->currentIndex--;
        return CR_OK;
    } else {
        return CR_END;
    }
}

void Uncompressed_FreeChunkIterator(ChunkIter_t *iterator) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->options & CHUNK_ITER_OP_FREE_CHUNK) {
        Uncompressed_FreeChunk(iter->chunk);
    }
    free(iter);
}

size_t Uncompressed_GetChunkSize(Chunk_t *chunk, bool includeStruct) {
    Chunk *uncompChunk = chunk;
    size_t size = uncompChunk->size;
    size += includeStruct ? sizeof(*uncompChunk) : 0;
    return size;
}

typedef void (*SaveUnsignedFunc)(void *, uint64_t);
typedef void (*SaveStringBufferFunc)(void *, const char *str, size_t len);
typedef uint64_t (*ReadUnsignedFunc)(void *);
typedef char *(*ReadStringBufferFunc)(void *, size_t *);

static void Uncompressed_GenericSerialize(Chunk_t *chunk,
                                          void *ctx,
                                          SaveUnsignedFunc saveUnsigned,
                                          SaveStringBufferFunc saveString,
                                          bool isBlob) {
    Chunk *uncompchunk = chunk;

    saveUnsigned(ctx, uncompchunk->base_timestamp);
    saveUnsigned(ctx, uncompchunk->num_samples);
    saveUnsigned(ctx, uncompchunk->size);

    saveString(ctx, (char *)uncompchunk->samples, uncompchunk->size);
    if (!isBlob)
        return;

    for (int ix = 0; ix < uncompchunk->num_samples; ix++) {
        Sample *sample = &uncompchunk->samples[ix];
        TSBlob *blob = VALUE_BLOB(&sample->value);
        RedisModule_SaveBlob(ctx, blob);
    }
}

static void Uncompressed_Deserialize(Chunk_t **chunk,
                                     void *ctx,
                                     ReadUnsignedFunc readUnsigned,
                                     ReadStringBufferFunc readStringBuffer,
                                     bool isBlob) {
    Chunk *uncompchunk = (Chunk *)malloc(sizeof(*uncompchunk));

    uncompchunk->base_timestamp = readUnsigned(ctx);
    uncompchunk->num_samples = readUnsigned(ctx);
    uncompchunk->size = readUnsigned(ctx);
    size_t string_buffer_size;
    uncompchunk->samples = (Sample *)readStringBuffer(ctx, &string_buffer_size);
    uncompchunk->isBlob = isBlob;
    *chunk = (Chunk_t *)uncompchunk;
    if (!isBlob)
        return;

    for (int ix = 0; ix < uncompchunk->num_samples; ix++) {
        Sample *sample = &uncompchunk->samples[ix];
        VALUE_BLOB(&sample->value) = RedisModule_LoadBlob(ctx);
    }

    for (Sample *sample = &uncompchunk->samples[uncompchunk->num_samples];
         (char *)sample < (char *)uncompchunk->samples + string_buffer_size;
         sample++) {
        VALUE_BLOB(&sample->value) = NewBlob(NULL, 0);
    }
}

void Uncompressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io, bool isBlob) {
    Uncompressed_GenericSerialize(chunk,
                                  io,
                                  (SaveUnsignedFunc)RedisModule_SaveUnsigned,
                                  (SaveStringBufferFunc)RedisModule_SaveStringBuffer,
                                  isBlob);
}

void Uncompressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io, bool isBlob) {
    Uncompressed_Deserialize(chunk,
                             io,
                             (ReadUnsignedFunc)RedisModule_LoadUnsigned,
                             (ReadStringBufferFunc)RedisModule_LoadStringBuffer,
                             isBlob);
}

void Uncompressed_GearsSerialize(Chunk_t *chunk, Gears_BufferWriter *bw) {}

void Uncompressed_GearsDeserialize(Chunk_t *chunk, Gears_BufferReader *br) {}
