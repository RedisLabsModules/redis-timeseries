/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#ifndef GENERIC__CHUNK_H
#define GENERIC__CHUNK_H

#include "consts.h"
#include "redisgears.h"

#include <stdio.h>  // printf
#include <stdlib.h> // malloc
#include <string.h> // memcpy, memmove
#include <sys/types.h>
#include <rmutil/strings.h>

struct RedisModuleIO;

typedef struct Sample
{
    timestamp_t timestamp;
    double value;
} Sample;

typedef void Chunk_t;
typedef void ChunkIter_t;

#define CHUNK_ITER_OP_NONE 0
#define CHUNK_ITER_OP_REVERSE 1
// This is supported *only* by uncompressed chunk, this is used when we reverse read a compressed
// chunk by uncompressing it into a *un*compressed chunk and returning a reverse iterator on that
// "temporary" uncompressed chunk.
#define CHUNK_ITER_OP_FREE_CHUNK 1 << 2

typedef enum CHUNK_TYPES_T
{
    CHUNK_REGULAR,
    CHUNK_COMPRESSED,
    CHUNK_COMPRESSED_TURBOGORILLA
} CHUNK_TYPES_T;

typedef struct UpsertCtx
{
    Sample sample;
    Chunk_t *inChunk; // original chunk
} UpsertCtx;

typedef struct ChunkIterFuncs
{
    void (*Free)(ChunkIter_t *iter);
    ChunkResult (*GetNext)(ChunkIter_t *iter, Sample *sample);
    ChunkResult (*GetPrev)(ChunkIter_t *iter, Sample *sample);
    void (*Reset)(ChunkIter_t *iter, Chunk_t *chunk);
} ChunkIterFuncs;

typedef struct ChunkFuncs
{
    Chunk_t *(*NewChunk)(size_t sampleCount);
    void (*FreeChunk)(Chunk_t *chunk);
    Chunk_t *(*CloneChunk)(Chunk_t *chunk);
    Chunk_t *(*SplitChunk)(Chunk_t *chunk);

    size_t (*DelRange)(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs);
    ChunkResult (*AddSample)(Chunk_t *chunk, Sample *sample);
    ChunkResult (*UpsertSample)(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy);

    ChunkIter_t *(*NewChunkIterator)(Chunk_t *chunk,
                                     int options,
                                     ChunkIterFuncs *retChunkIterClass);

    size_t (*GetChunkSize)(Chunk_t *chunk, bool includeStruct);
    u_int64_t (*GetNumOfSample)(Chunk_t *chunk);
    u_int64_t (*GetLastTimestamp)(Chunk_t *chunk);
    u_int64_t (*GetFirstTimestamp)(Chunk_t *chunk);

    void (*SaveToRDB)(Chunk_t *chunk, struct RedisModuleIO *io);
    void (*LoadFromRDB)(Chunk_t **chunk, struct RedisModuleIO *io);
    void (*GearsSerialize)(Chunk_t *chunk, Gears_BufferWriter *bw);
    void (*GearsDeserialize)(Chunk_t **chunk, Gears_BufferReader *br);
} ChunkFuncs;

ChunkResult handleDuplicateSample(DuplicatePolicy policy,
                                  const double oldSample,
                                  double *newSample);
const char *DuplicatePolicyToString(DuplicatePolicy policy);
int RMStringLenDuplicationPolicyToEnum(RedisModuleString *aggTypeStr);
DuplicatePolicy DuplicatePolicyFromString(const char *input, size_t len);

ChunkFuncs *GetChunkClass(CHUNK_TYPES_T chunkClass);
ChunkIterFuncs *GetChunkIteratorClass(CHUNK_TYPES_T chunkType);

#endif // GENERIC__CHUNK_H
