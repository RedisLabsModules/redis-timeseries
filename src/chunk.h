/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef CHUNK_H
#define CHUNK_H

#include "consts.h"
#include "generic_chunk.h"

#include <sys/types.h>

typedef struct Chunk
{
    timestamp_t base_timestamp;
    Sample *samples;
    unsigned int num_samples;
    size_t size;
    bool isBlob;
} Chunk;

typedef struct ChunkIterator
{
    Chunk *chunk;
    int currentIndex;
    timestamp_t lastTimestamp;
    int lastValue;
    int options;
} ChunkIterator;

Chunk_t *Uncompressed_NewChunk(bool isBlob, size_t sampleCount);
void Uncompressed_FreeChunk(Chunk_t *chunk);

/**
 * TODO: describe me
 * @param chunk
 * @return
 */
Chunk_t *Uncompressed_SplitChunk(Chunk_t *chunk);
size_t Uncompressed_GetChunkSize(Chunk_t *chunk, bool includeStruct);

/**
 * TODO: describe me
 * 0 for failure, 1 for success
 * @param chunk
 * @param sample
 * @return
 */
ChunkResult Uncompressed_AddSample(Chunk_t *chunk, Sample *sample);

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult Uncompressed_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy);
size_t Uncompressed_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs);

u_int64_t Uncompressed_NumOfSample(Chunk_t *chunk);
timestamp_t Uncompressed_GetLastTimestamp(Chunk_t *chunk);
timestamp_t Uncompressed_GetFirstTimestamp(Chunk_t *chunk);

ChunkIter_t *Uncompressed_NewChunkIterator(Chunk_t *chunk,
                                           int options,
                                           ChunkIterFuncs *retChunkIterClass);
ChunkResult Uncompressed_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample);
ChunkResult Uncompressed_ChunkIteratorGetPrev(ChunkIter_t *iterator, Sample *sample);
void Uncompressed_FreeChunkIterator(ChunkIter_t *iter);

// RDB
void Uncompressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io, bool blob);
void Uncompressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io, bool blob);

// Gears
void Uncompressed_GearsSerialize(Chunk_t *chunk, Gears_BufferWriter *bw);
void Uncompressed_GearsDeserialize(Chunk_t *chunk, Gears_BufferReader *br);

#endif
