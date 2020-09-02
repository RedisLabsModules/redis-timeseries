/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#ifndef GENERIC__CHUNK_H
#define GENERIC__CHUNK_H

#include <sys/types.h>
#include <stdlib.h>         // malloc
#include <stdio.h>          // printf
#include <string.h>         // memcpy, memmove
#include "consts.h"

struct RedisModuleIO;

typedef struct Sample {
    timestamp_t timestamp;
    double value;
} Sample;

typedef void Chunk_t;

typedef struct {
    timestamp_t baseTimestamp;
    short numSamples;
    short size;
    short type;
    struct ChunkFuncs *funcs;
} Chunk_base;

typedef void ChunkIter_t;

#define CHUNK_ITER_OP_NONE 0
#define CHUNK_ITER_OP_REVERSE 1
// This is supported *only* by uncompressed chunk, this is used when we reverse read a compressed chunk by
// uncompressing it into a *un*compressed chunk and returning a reverse iterator on that "temporary" uncompressed chunk.
#define CHUNK_ITER_OP_FREE_CHUNK 1 << 2

typedef enum {
    CHUNK_REGULAR,
    CHUNK_COMPRESSED 
} CHUNK_TYPES_T;

typedef struct UpsertCtx {
    Sample sample;
    Chunk_t *inChunk;       // original chunk  
} UpsertCtx;

typedef struct ChunkFuncs {
    Chunk_t *(*NewChunk)(size_t sampleCount);
    void(*FreeChunk)(Chunk_t *chunk);
    Chunk_t *(*SplitChunk)(Chunk_t *chunk);

    ChunkResult(*AddSample)(Chunk_t *chunk, Sample *sample);
    ChunkResult(*UpsertSample)(UpsertCtx *uCtx, int *size);

    ChunkIter_t *(*NewChunkIterator)(Chunk_t *chunk, int options);
    void(*FreeChunkIterator)(ChunkIter_t *iter);
    ChunkResult(*ChunkIteratorGetNext)(ChunkIter_t *iter, Sample *sample);
    ChunkResult(*ChunkIteratorGetPrev)(ChunkIter_t *iter, Sample *sample);

    size_t(*GetChunkSize)(Chunk_t *chunk, bool includeStruct);
    u_int64_t(*GetLastTimestamp)(Chunk_t *chunk);
    u_int64_t(*GetFirstTimestamp)(Chunk_t *chunk);

    void (*SaveToRDB)(Chunk_t *chunk, struct RedisModuleIO *io);
    void (*LoadFromRDB)(Chunk_t **chunk, struct RedisModuleIO *io);
} ChunkFuncs;

ChunkFuncs *GetChunkClass(CHUNK_TYPES_T chunkClass);

inline static timestamp_t GetFirstTimestamp(Chunk_t *chunk) {
    return ((Chunk_base *)chunk)->baseTimestamp;
}

inline static short GetNumOfSample(Chunk_t *chunk) {
    return ((Chunk_base *)chunk)->numSamples;
}

inline static ChunkFuncs *GetChunkFuncs(Chunk_t *chunk) {
    return ((Chunk_base *)chunk)->funcs;
}

#endif //GENERIC__CHUNK_H
