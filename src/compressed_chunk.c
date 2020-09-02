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
#define CHUNK_RESIZE_STEP 32

/*********************
 *  Chunk functions  *
 *********************/
Chunk_t *Compressed_NewChunk(size_t size) {
    CompressedChunk *chunk = (CompressedChunk *)calloc(1, sizeof(CompressedChunk));
    chunk->base.size = size;
    chunk->base.type = CHUNK_COMPRESSED;
    chunk->base.funcs = GetChunkClass(CHUNK_COMPRESSED);
    chunk->data = (u_int64_t *)calloc(chunk->base.size, sizeof(char));
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

static void ensureAddSample(CompressedChunk *chunk, Sample *sample) {
    ChunkResult res = Compressed_AddSample(chunk, sample);
    if (res != CR_OK) {
        int oldsize = chunk->base.size;
        chunk->base.size += CHUNK_RESIZE_STEP;
        chunk->data = (u_int64_t *)realloc(chunk->data, chunk->base.size * sizeof(char));
        memset((char *)chunk->data + oldsize, 0, CHUNK_RESIZE_STEP);
        // printf("Chunk extended to %lu \n", chunk->size);
        res = Compressed_AddSample(chunk, sample);
        assert(res == CR_OK);
    }
}

static void trimChunk(CompressedChunk *chunk) {
    int excess = (chunk->base.size * BIT - chunk->idx) / BIT;

    assert(excess >= 0); // else we have written beyond allocated memory

    if (excess > 1) {
        size_t newSize = chunk->base.size - excess + 1;
        // align to 8 bytes (u_int64_t) otherwise we will have an heap overflow in gorilla.c because
        // each write happens in 8 bytes blocks.
        newSize += sizeof(binary_t) - (newSize % sizeof(binary_t));
        chunk->data = realloc(chunk->data, newSize);
        chunk->base.size = newSize;
    }
}

Chunk_t *Compressed_SplitChunk(Chunk_t *chunk) {
    CompressedChunk *curChunk = chunk;
    size_t split = curChunk->base.numSamples / 2;
    size_t curNumSamples = curChunk->base.numSamples - split;

    // add samples in new chunks
    size_t i = 0;
    Sample sample;
    ChunkIter_t *iter = Compressed_NewChunkIterator(curChunk, CHUNK_ITER_OP_NONE);
    CompressedChunk *newChunk1 = Compressed_NewChunk(curChunk->base.size);
    CompressedChunk *newChunk2 = Compressed_NewChunk(curChunk->base.size);
    for (; i < curNumSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        ensureAddSample(newChunk1, &sample);
    }
    for (; i < curChunk->base.numSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        ensureAddSample(newChunk2, &sample);
    }

    trimChunk(newChunk1);
    trimChunk(newChunk2);
    swapChunks(curChunk, newChunk1);

    Compressed_FreeChunkIterator(iter);
    Compressed_FreeChunk(newChunk1);

    return newChunk2;
}

ChunkResult Compressed_UpsertSample(UpsertCtx *uCtx, int *size) {
    *size = 0;
    ChunkResult rv = CR_OK;
    ChunkResult nextRes = CR_OK;
    CompressedChunk *oldChunk = (CompressedChunk *)uCtx->inChunk;

    size_t newSize = oldChunk->base.size;

    CompressedChunk *newChunk = Compressed_NewChunk(newSize);
    Compressed_Iterator *iter = Compressed_NewChunkIterator(oldChunk, CHUNK_ITER_OP_NONE);
    timestamp_t ts = uCtx->sample.timestamp;
    int numSamples = oldChunk->base.numSamples;

    size_t i = 0;
    Sample iterSample;
    for (; i < numSamples; ++i) {
        nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        if (iterSample.timestamp >= ts) {
            break;
        }
        ensureAddSample(newChunk, &iterSample);
    }

    if (ts == iterSample.timestamp) {
        nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        *size = -1; // we skipped a sample
    }
    // upsert the sample
    ensureAddSample(newChunk, &uCtx->sample);
    *size += 1;

    if (i < numSamples) {
        while (nextRes == CR_OK) {
            ensureAddSample(newChunk, &iterSample);
            nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        }
    }

    swapChunks(newChunk, oldChunk);

    Compressed_FreeChunkIterator(iter);
    Compressed_FreeChunk(newChunk);
    return rv;
}

ChunkResult Compressed_AddSample(Chunk_t *chunk, Sample *sample) {
    return Compressed_Append((CompressedChunk *)chunk, sample->timestamp, sample->value);
}

u_int64_t Compressed_ChunkNumOfSample(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->base.numSamples;
}

timestamp_t Compressed_GetFirstTimestamp(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->base.baseTimestamp;
}

timestamp_t Compressed_GetLastTimestamp(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->prevTimestamp;
}

size_t Compressed_GetChunkSize(Chunk_t *chunk, bool includeStruct) {
    CompressedChunk *cmpChunk = chunk;
    size_t size = cmpChunk->base.size * sizeof(char);
    size += includeStruct ? sizeof(*cmpChunk) : 0;
    return size;
}

static Chunk *decompressChunk(CompressedChunk *compressedChunk) {
    Sample sample;
    uint64_t numSamples = compressedChunk->base.numSamples;
    Chunk *uncompressedChunk = Uncompressed_NewChunk(numSamples * SAMPLE_SIZE);

    ChunkIter_t *iter = Compressed_NewChunkIterator(compressedChunk, CHUNK_ITER_OP_NONE);
    for (uint64_t i = 0; i < numSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        Uncompressed_AddSample(uncompressedChunk, &sample);
    }
    Compressed_FreeChunkIterator(iter);
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

ChunkIter_t *Compressed_NewChunkIterator(Chunk_t *chunk, int options) {
    CompressedChunk *compressedChunk = chunk;

    // for reverse iterator of compressed chunks
    if (options & CHUNK_ITER_OP_REVERSE) {
        int uncompressed_options = CHUNK_ITER_OP_REVERSE | CHUNK_ITER_OP_FREE_CHUNK;
        Chunk *uncompressedChunk = decompressChunk(compressedChunk);
        return Uncompressed_NewChunkIterator(uncompressedChunk, uncompressed_options);
    }

    Compressed_Iterator *iter = (Compressed_Iterator *)calloc(1, sizeof(Compressed_Iterator));

    iter->chunk = compressedChunk;
    iter->idx = 0;
    iter->count = 0;

    iter->prevTS = compressedChunk->base.baseTimestamp;
    iter->prevDelta = 0;

    iter->prevValue.d = compressedChunk->baseValue.d;
    iter->prevLeading = 32;
    iter->prevTrailing = 32;

    return (ChunkIter_t *)iter;
}

ChunkResult Compressed_ChunkIteratorGetNext(ChunkIter_t *iter, Sample *sample) {
    return Compressed_ReadNext((Compressed_Iterator *)iter, &sample->timestamp, &sample->value);
}

void Compressed_FreeChunkIterator(ChunkIter_t *iter) {
    free(iter);
}

void Compressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io) {
    CompressedChunk *compchunk = chunk;

    RedisModule_SaveUnsigned(io, compchunk->base.size);
    RedisModule_SaveUnsigned(io, compchunk->base.numSamples);
    RedisModule_SaveUnsigned(io, compchunk->idx);
    RedisModule_SaveUnsigned(io, compchunk->baseValue.u);
    RedisModule_SaveUnsigned(io, compchunk->base.baseTimestamp);
    RedisModule_SaveUnsigned(io, compchunk->prevTimestamp);
    RedisModule_SaveSigned(io, compchunk->prevTimestampDelta);
    RedisModule_SaveUnsigned(io, compchunk->prevValue.u);
    RedisModule_SaveUnsigned(io, compchunk->prevLeading);
    RedisModule_SaveUnsigned(io, compchunk->prevTrailing);
    RedisModule_SaveStringBuffer(io, (char *)compchunk->data, compchunk->base.size);
}

void Compressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io) {
    CompressedChunk *compchunk = (CompressedChunk *)malloc(sizeof(*compchunk));

    compchunk->base.size = RedisModule_LoadUnsigned(io);
    compchunk->base.numSamples = RedisModule_LoadUnsigned(io);
    compchunk->idx = RedisModule_LoadUnsigned(io);
    compchunk->baseValue.u = RedisModule_LoadUnsigned(io);
    compchunk->base.baseTimestamp = RedisModule_LoadUnsigned(io);
    compchunk->prevTimestamp = RedisModule_LoadUnsigned(io);
    compchunk->prevTimestampDelta = RedisModule_LoadSigned(io);
    compchunk->prevValue.u = RedisModule_LoadUnsigned(io);
    compchunk->prevLeading = RedisModule_LoadUnsigned(io);
    compchunk->prevTrailing = RedisModule_LoadUnsigned(io);

    compchunk->data = (uint64_t *)RedisModule_LoadStringBuffer(io, NULL);
    *chunk = (Chunk_t *)compchunk;
}