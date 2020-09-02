/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#ifndef GORILLA_H
#define GORILLA_H

#include <sys/types.h>      // u_int_t
#include <stdbool.h>        // bool
#include "consts.h"
#include "generic_chunk.h"

typedef u_int64_t timestamp_t;
typedef u_int64_t binary_t;
typedef u_int64_t globalbit_t;
typedef u_int8_t localbit_t;

typedef union {
    double d;
    int64_t i;
    u_int64_t u;
} union64bits;

typedef struct CompressedChunk {
    Chunk_base base;
    u_int64_t idx;

    union64bits baseValue;

    u_int64_t *data;
    
    u_int64_t prevTimestamp;
    int64_t prevTimestampDelta;

    union64bits prevValue;
    u_int8_t prevLeading;
    u_int8_t prevTrailing;
} CompressedChunk;

typedef struct Compressed_Iterator {
  CompressedChunk *chunk;
  u_int64_t idx;
  u_int64_t count;

  // timestamp vars
  u_int64_t prevTS;
  int64_t prevDelta;

  // value vars
  union64bits prevValue;  
  u_int8_t prevLeading;
  u_int8_t prevTrailing;
} Compressed_Iterator;

ChunkResult Compressed_Append(CompressedChunk *chunk, u_int64_t timestamp, double value);
ChunkResult Compressed_ReadNext(Compressed_Iterator *iter, u_int64_t *timestamp, double *value);

#endif
