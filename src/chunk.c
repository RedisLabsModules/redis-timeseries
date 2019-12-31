/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#include "chunk.h"
#include <string.h>
#include "rmutil/alloc.h"

Chunk_t *U_NewChunk(size_t sampleCount) {
    Chunk *newChunk = (Chunk *)malloc(sizeof(Chunk));
    newChunk->num_samples = 0;
    newChunk->max_samples = sampleCount;
    newChunk->nextChunk = NULL;
    newChunk->samples = (Sample *)malloc(sizeof(Sample) * sampleCount);

    return newChunk;
}

void U_FreeChunk(Chunk_t *chunk) {
    free(((Chunk *)chunk)->samples);
    free(chunk);
}

static int IsChunkFull(Chunk *chunk) {
    return chunk->num_samples == chunk->max_samples;
}

u_int64_t U_NumOfSample(Chunk_t *chunk) {
    return ((Chunk *)chunk)->num_samples;
}

static Sample *ChunkGetSampleArray(Chunk *chunk) {
    return chunk->samples;
}

static Sample *ChunkGetSample(Chunk *chunk, int index) {
    return &ChunkGetSampleArray(chunk)[index];
}

timestamp_t U_GetLastTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->num_samples == 0) {
        return -1;
    }
    return ChunkGetSample(chunk, ((Chunk *)chunk)->num_samples - 1)->timestamp;
}

timestamp_t U_GetFirstTimestamp(Chunk_t *chunk) {
    if (((Chunk *)chunk)->num_samples == 0) {
        return -1;
    }
    return ChunkGetSample(chunk, 0)->timestamp;
}

ChunkResult U_AddSample(Chunk_t *chunk, Sample *sample) {
    Chunk *regChunk = (Chunk *)chunk;
    if (IsChunkFull(regChunk)){
        return CR_END;
    }

    if (U_NumOfSample(regChunk) == 0) {
        // initialize base_timestamp
        regChunk->base_timestamp = sample->timestamp;
    }

    ChunkGetSampleArray(regChunk)[regChunk->num_samples] = *sample;
    regChunk->num_samples++;

    return CR_OK;
}

ChunkIter_t *U_NewChunkIterator(Chunk_t *chunk) {
    ChunkIterator *iter = (ChunkIterator *)calloc(1, sizeof(ChunkIterator));
    iter->chunk = chunk;
    iter->currentIndex = 0;
    return iter;
}

ChunkResult U_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = iterator;
    if (iter->currentIndex < iter->chunk->num_samples) {
        iter->currentIndex++;
        Sample *internalSample = ChunkGetSample(iter->chunk, iter->currentIndex - 1);
        memcpy(sample, internalSample, sizeof(Sample));
        return CR_OK;
    } else {
        return CR_END;
    }
}

size_t U_GetChunkSize(Chunk_t *chunk) {
    return sizeof(Chunk) + ((Chunk *)chunk)->max_samples * sizeof(Sample);
}