/*
 * Copyright 2018-2020 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "compressed_chunk.h"

#include "chunk.h"

#include <assert.h> // assert
#include <limits.h>
#include <stdio.h>  // printf
#include <stdlib.h> // malloc
#include "rmutil/alloc.h"

#define BIT 8
#define EXTRA_SPACE 1024

#define CHECK_EXTEND_CHUNK(res, chunk, sample)                                                     \
    if (res != CR_OK) {                                                                            \
        extendChunk(chunk);                                                                        \
        Compressed_AddSample(chunk, &sample);                                                      \
    }

/*********************
 *  Chunk functions  *
 *********************/
Chunk_t *Compressed_NewChunk(size_t size) {
    CompressedChunk *chunk = (CompressedChunk *)calloc(1, sizeof(CompressedChunk));
    chunk->size = size * sizeof(Sample);
    chunk->data = (u_int64_t *)calloc(chunk->size, sizeof(char));
    chunk->prevLeading = 32;
    chunk->prevTrailing = 32;
    return chunk;
}

void Compressed_FreeChunk(Chunk_t *chunk) {
    CompressedChunk *cmpChunk = chunk;
    free(cmpChunk->data);
    cmpChunk->data = NULL;
    free(chunk);
}

static void swapChunks(CompressedChunk *a, CompressedChunk *b) {
    CompressedChunk tmp = *a;
    *a = *b;
    *b = tmp;
}

static void extendChunk(CompressedChunk *chunk) {
    chunk->size += 64;
    chunk->data = (u_int64_t *)realloc(chunk->data, chunk->size * sizeof(char));
}

static void trimChunk(CompressedChunk *chunk) {
    size_t excess = chunk->size - (chunk->idx + BIT) / BIT;

    assert(excess >= 0); // else we have written beyond allocated memory

    if (excess > 0) {
        size_t newSize = chunk->size - excess;
        chunk->data = realloc(chunk->data, newSize);
        chunk->size = newSize;
    }
}

Chunk_t *Compressed_SplitChunk(Chunk_t *chunk) {
    CompressedChunk *curChunk = chunk;
    size_t split = curChunk->count / 2;
    size_t curNumSamples = curChunk->count - split;

    // add samples in new chunks
    size_t i = 0;
    Sample sample;
    ChunkIter_t *iter = Compressed_NewChunkIterator(curChunk, false);
    CompressedChunk *newChunk1 = Compressed_NewChunk(curChunk->size / sizeof(Sample));
    CompressedChunk *newChunk2 = Compressed_NewChunk(curChunk->size / sizeof(Sample));
    for (; i < curNumSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        if (Compressed_AddSample(newChunk1, &sample) != CR_OK) {
            goto error;
        }
    }
    for (; i < curChunk->count; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        if (Compressed_AddSample(newChunk2, &sample) != CR_OK) {
            goto error;
        }
    }

    trimChunk(newChunk1);
    trimChunk(newChunk2);
    swapChunks(curChunk, newChunk1);

    Compressed_FreeChunkIterator(iter, false);
    Compressed_FreeChunk(newChunk1);

    return newChunk2;

error:
    Compressed_FreeChunkIterator(iter, false);
    Compressed_FreeChunk(newChunk1);
    Compressed_FreeChunk(newChunk2);
    return NULL;
}

ChunkResult Compressed_UpsertSample(AddCtx *aCtx) {
    ChunkResult rv = CR_OK;
    ChunkResult res = CR_OK;
    CompressedChunk *oldChunk = (CompressedChunk *)aCtx->inChunk;

    short newSize = oldChunk->size / sizeof(Sample);
    // extend size if approaching end
    if (((oldChunk->idx) / BIT) + EXTRA_SPACE >= oldChunk->size) {
        newSize += EXTRA_SPACE / sizeof(Sample);
    };

    CompressedChunk *newChunk = Compressed_NewChunk(newSize);
    Compressed_Iterator *iter = Compressed_NewChunkIterator(oldChunk, false);
    timestamp_t ts = aCtx->sample.timestamp;
    int numSamples = oldChunk->count;

    size_t i = 0;
    Sample iterSample;
    for (; i < numSamples; ++i) {
        res = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        if (iterSample.timestamp >= ts) {
            break;
        }
        res = Compressed_AddSample(newChunk, &iterSample);
        CHECK_EXTEND_CHUNK(res, newChunk, iterSample);
    }

    if (ts == iterSample.timestamp) {
        res = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        aCtx->sz = -1;                     // we skipped a sample
    } else if (aCtx->type == UPSERT_DEL) { // No sample to delete
        rv = CR_DEL_FAIL;
        goto clean;
    }

    if (aCtx->type != UPSERT_DEL) {
        ChunkResult resSample = Compressed_AddSample(newChunk, &aCtx->sample);
        CHECK_EXTEND_CHUNK(resSample, newChunk, aCtx->sample);
        aCtx->sz += 1;
    }

    if (i != numSamples) { // sample is not last
        while (res == CR_OK) {
            res = Compressed_AddSample(newChunk, &iterSample);
            CHECK_EXTEND_CHUNK(res, newChunk, iterSample);
            res = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        }
    }

    // trim data
    if (!aCtx->latestChunk) {
        int excess = newChunk->size - (newChunk->idx + 8) / 8;
        assert(excess >= 0);
        if (excess > 0) {
            newSize = newChunk->size - excess;
            newChunk->data = realloc(newChunk->data, newSize);
            newChunk->size = newSize;
        }
    }
    swapChunks(newChunk, oldChunk);
clean:
    Compressed_FreeChunkIterator(iter, false);
    Compressed_FreeChunk(newChunk);
    return rv;
}

ChunkResult Compressed_AddSample(Chunk_t *chunk, Sample *sample) {
    return Compressed_Append((CompressedChunk *)chunk, sample->timestamp, sample->value);
}

u_int64_t Compressed_ChunkNumOfSample(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->count;
}

timestamp_t Compressed_GetFirstTimestamp(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->baseTimestamp;
}

timestamp_t Compressed_GetLastTimestamp(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->prevTimestamp;
}

size_t Compressed_GetChunkSize(Chunk_t *chunk) {
    CompressedChunk *cmpChunk = chunk;
    return sizeof(*cmpChunk) + cmpChunk->size * sizeof(char);
}

static Chunk *decompressChunk(CompressedChunk *compressedChunk) {
    Sample sample;
    uint64_t numSamples = compressedChunk->count;
    Chunk *uncompressedChunk = Uncompressed_NewChunk(numSamples);

    ChunkIter_t *iter = Compressed_NewChunkIterator(compressedChunk, 0);
    for (uint64_t i = 0; i < numSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        Uncompressed_AddSample(uncompressedChunk, &sample);
    }
    Compressed_FreeChunkIterator(iter, false);
    return uncompressedChunk;
}

/************************
 *  Iterator functions  *
 ************************/
// LCOV_EXCL_START - used for debug
u_int64_t getIterIdx(ChunkIter_t *iter) {
    return ((Compressed_Iterator *)iter)->idx;
}
// LCOV_EXCL_STOP

ChunkIter_t *Compressed_NewChunkIterator(Chunk_t *chunk, bool rev) {
    CompressedChunk *compressedChunk = chunk;

    // for reverse iterator of compressed chunks
    if (rev == true) {
        Chunk *uncompressedChunk = decompressChunk(compressedChunk);
        return Uncompressed_NewChunkIterator(uncompressedChunk, true);
    }

    Compressed_Iterator *iter = (Compressed_Iterator *)calloc(1, sizeof(Compressed_Iterator));

    iter->chunk = compressedChunk;
    iter->idx = 0;
    iter->count = 0;

    iter->prevTS = compressedChunk->baseTimestamp;
    iter->prevDelta = 0;

    iter->prevValue.d = compressedChunk->baseValue.d;
    iter->prevLeading = 32;
    iter->prevTrailing = 32;

    return (ChunkIter_t *)iter;
}

ChunkResult Compressed_ChunkIteratorGetNext(ChunkIter_t *iter, Sample *sample) {
    return Compressed_ReadNext((Compressed_Iterator *)iter, &sample->timestamp, &sample->value);
}

void Compressed_FreeChunkIterator(ChunkIter_t *iter, bool rev) {
    // compressed iterator on reverse query has to release decompressed chunk
    if (rev) {
        free(((ChunkIterator *)iter)->chunk);
    }
    free(iter);
}
