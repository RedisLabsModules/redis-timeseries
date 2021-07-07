/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef TURBOGORILLA_CHUNK_H
#define TURBOGORILLA_CHUNK_H

#include "consts.h"
#include "generic_chunk.h"

#include <sys/types.h>

#define TURBOGORILLA_UNCOMPRESSED_SIZE sizeof(uint64_t) + sizeof(double)

typedef struct TurboGorilla_Chunk TurboGorilla_Chunk;

typedef struct TurboGorilla_ChunkIterator TurboGorilla_ChunkIterator;

Chunk_t *TurboGorilla_NewChunk(size_t sampleCount);
void TurboGorilla_FreeChunk(Chunk_t *chunk);
Chunk_t *TurboGorilla_CloneChunk(Chunk_t *chunk);

/**
 * TODO: describe me
 * @param chunk
 * @return
 */
Chunk_t *TurboGorilla_SplitChunk(Chunk_t *chunk);
size_t TurboGorilla_GetChunkSize(Chunk_t *chunk, bool includeStruct);

ChunkResult TurboGorilla_AddSampleOptimized(Chunk_t *chunk, u_int64_t timestamp, double value);

/**
 * TODO: describe me
 * 0 for failure, 1 for success
 * @param chunk
 * @param sample
 * @return
 */
ChunkResult TurboGorilla_AddSample(Chunk_t *chunk, Sample *sample);

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult TurboGorilla_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy);
size_t TurboGorilla_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs);

u_int64_t TurboGorilla_NumOfSample(Chunk_t *chunk);
timestamp_t TurboGorilla_GetLastTimestamp(Chunk_t *chunk);
timestamp_t TurboGorilla_GetFirstTimestamp(Chunk_t *chunk);

ChunkIter_t *TurboGorilla_NewChunkIterator(Chunk_t *chunk,
                                           int options,
                                           ChunkIterFuncs *retChunkIterClass);
void TurboGorilla_ResetChunkIterator(ChunkIter_t *iterator, Chunk_t *chunk);
ChunkResult TurboGorilla_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample);
ChunkResult TurboGorilla_ChunkIteratorGetPrev(ChunkIter_t *iterator, Sample *sample);
void TurboGorilla_FreeChunkIterator(ChunkIter_t *iter);

// RDB
void TurboGorilla_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io);
void TurboGorilla_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io);

// Gears
void TurboGorilla_GearsSerialize(Chunk_t *chunk, Gears_BufferWriter *bw);
void TurboGorilla_GearsDeserialize(Chunk_t *chunk, Gears_BufferReader *br);

#endif
