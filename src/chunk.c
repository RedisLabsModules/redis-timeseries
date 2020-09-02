/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "chunk.h"

#include "rmutil/alloc.h"

Chunk_t *Uncompressed_NewChunk(size_t size) {
    Chunk *newChunk = (Chunk *)malloc(sizeof(Chunk));
    newChunk->base.numSamples = 0;
    newChunk->base.size = size;
    newChunk->base.type = CHUNK_REGULAR;
    newChunk->base.funcs = GetChunkClass(CHUNK_REGULAR);
    newChunk->samples = (Sample *)malloc(size);

    return newChunk;
}

void Uncompressed_FreeChunk(Chunk_t *chunk) {
    free(((Chunk *)chunk)->samples);
    free(chunk);
}

/**
 * TODO: describe me
 * @param chunk
 * @return
 */
Chunk_t *Uncompressed_SplitChunk(Chunk_t *chunk) {
    Chunk *curChunk = (Chunk *)chunk;
    size_t split = curChunk->base.numSamples / 2;
    size_t curNumSamples = curChunk->base.numSamples - split;

    // create chunk and copy samples
    Chunk *newChunk = Uncompressed_NewChunk(split * SAMPLE_SIZE);
    for (size_t i = 0; i < split; ++i) {
        Sample *sample = &curChunk->samples[curNumSamples + i];
        Uncompressed_AddSample(newChunk, sample);
    }

    // update current chunk
    curChunk->base.numSamples = curNumSamples;
    curChunk->base.size = curNumSamples * SAMPLE_SIZE;
    curChunk->samples = realloc(curChunk->samples, curChunk->base.size);

    return newChunk;
}

static int IsChunkFull(Chunk *chunk) {
    return chunk->base.numSamples == chunk->base.size / SAMPLE_SIZE;
}

u_int64_t Uncompressed_NumOfSample(Chunk_t *chunk) {
    return ((Chunk *)chunk)->base.numSamples;
}

static Sample *ChunkGetSample(Chunk *chunk, int index) {
    return &chunk->samples[index];
}

timestamp_t Uncompressed_GetLastTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->base.numSamples == 0) {
        return -1;
    }
    return ChunkGetSample(chunk, ((Chunk *)chunk)->base.numSamples - 1)->timestamp;
}

timestamp_t Uncompressed_GetFirstTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->base.numSamples == 0) {
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
        // initialize baseTimestamp
        regChunk->base.baseTimestamp = sample->timestamp;
    }

    regChunk->samples[regChunk->base.numSamples] = *sample;
    regChunk->base.numSamples++;

    return CR_OK;
}

/**
 * TODO: describe me
 * @param chunk
 * @param idx
 * @param sample
 */
static void upsertChunk(Chunk *chunk, size_t idx, Sample *sample) {
    if (chunk->base.numSamples == chunk->base.size / SAMPLE_SIZE) {
        chunk->base.size += sizeof(Sample);
        chunk->samples = realloc(chunk->samples, chunk->base.size);
    }
    if (idx < chunk->base.numSamples) { // sample is not last
        memmove(&chunk->samples[idx + 1],
                &chunk->samples[idx],
                (chunk->base.numSamples - idx) * sizeof(Sample));
    }
    chunk->samples[idx] = *sample;
    chunk->base.numSamples++;
}

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult Uncompressed_UpsertSample(UpsertCtx *uCtx, int *size) {
    *size = 0;
    Chunk *regChunk = (Chunk *)uCtx->inChunk;
    timestamp_t ts = uCtx->sample.timestamp;
    short numSamples = regChunk->base.numSamples;
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
    if (ts == sample->timestamp) {
        regChunk->samples[i].value = uCtx->sample.value;
        return CR_OK;
    }

    if (i == 0) {
        regChunk->base.baseTimestamp = ts;
    }

    upsertChunk(regChunk, i, &uCtx->sample);
    *size = 1;
    return CR_OK;
}

ChunkIter_t *Uncompressed_NewChunkIterator(Chunk_t *chunk, int options) {
    ChunkIterator *iter = (ChunkIterator *)calloc(1, sizeof(ChunkIterator));
    iter->chunk = chunk;
    if (options & CHUNK_ITER_OP_REVERSE) { // iterate from last to first
        iter->currentIndex = iter->chunk->base.numSamples - 1;
    } else { // iterate from first to last
        iter->currentIndex = 0;
    }
    return (ChunkIter_t *)iter;
}

ChunkResult Uncompressed_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->currentIndex < iter->chunk->base.numSamples) {
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
        free(iter->chunk);
    }
    free(iter);
}

size_t Uncompressed_GetChunkSize(Chunk_t *chunk, bool includeStruct) {
    Chunk *uncompChunk = chunk;
    size_t size = uncompChunk->base.size;
    size += includeStruct ? sizeof(*uncompChunk) : 0;
    return size;
}

void Uncompressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io) {
    Chunk *uncompchunk = chunk;

    RedisModule_SaveUnsigned(io, uncompchunk->base.baseTimestamp);
    RedisModule_SaveUnsigned(io, uncompchunk->base.numSamples);
    RedisModule_SaveUnsigned(io, uncompchunk->base.size);

    RedisModule_SaveStringBuffer(io, (char *)uncompchunk->samples, uncompchunk->base.size);
}

void Uncompressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io) {
    Chunk *uncompchunk = (Chunk *)malloc(sizeof(*uncompchunk));

    uncompchunk->base.baseTimestamp = RedisModule_LoadUnsigned(io);
    uncompchunk->base.numSamples = RedisModule_LoadUnsigned(io);
    uncompchunk->base.size = RedisModule_LoadUnsigned(io);

    uncompchunk->samples = (Sample *)RedisModule_LoadStringBuffer(io, NULL);
    *chunk = (Chunk_t *)uncompchunk;
}
