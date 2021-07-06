/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#ifndef CONSTS_H
#define CONSTS_H

#include <sys/types.h>
#include <stdbool.h>

#define TRUE 1
#define FALSE 0

#define SAMPLE_SIZE sizeof(Sample)

#define timestamp_t u_int64_t
#define api_timestamp_t u_int64_t
#define TSDB_ERR_TIMESTAMP_TOO_OLD -1
#define TSDB_OK 0
#define TSDB_ERROR -1
#define TSDB_NOTEXISTS 2
#define TSDB_ERR_TIMESTAMP_OCCUPIED -2

/* TS.CREATE Defaults */
#define RETENTION_TIME_DEFAULT          0LL
#define Chunk_SIZE_BYTES_SECS           4096LL   // fills one page 4096
#define SPLIT_FACTOR                    1.2
#define DEFAULT_DUPLICATE_POLICY        DP_BLOCK

/* TS.Range Aggregation types */
typedef enum {
    TS_AGG_INVALID = -1,
    TS_AGG_NONE = 0,
    TS_AGG_MIN,
    TS_AGG_MAX,
    TS_AGG_SUM,
    TS_AGG_AVG,
    TS_AGG_COUNT,
    TS_AGG_FIRST,
    TS_AGG_LAST,
    TS_AGG_RANGE,
    TS_AGG_STD_P,
    TS_AGG_STD_S,
    TS_AGG_VAR_P,
    TS_AGG_VAR_S,
    TS_AGG_BLOB_COUNT,
    TS_AGG_BLOB_FIRST,
    TS_AGG_BLOB_LAST,
    TS_AGG_TYPES_MAX // 16
} TS_AGG_TYPES_T;


typedef enum DuplicatePolicy {
    DP_INVALID = -1,
    DP_NONE = 0,
    DP_BLOCK = 1,
    DP_LAST = 2,
    DP_FIRST = 3,
    DP_MIN = 4,
    DP_MAX = 5,
    DP_SUM = 6,
} DuplicatePolicy;

/* Series struct options */
#define SERIES_OPT_UNCOMPRESSED 0x1
#define SERIES_OPT_BLOB 0x2

/* Chunk enum */
typedef enum {
  CR_OK = 0,    // RM_OK
  CR_ERR = 1,   // RM_ERR
  CR_END = 2,   // END_OF_CHUNK
} ChunkResult;

/* parsing */

#define DUPLICATE_POLICY_ARG "DUPLICATE_POLICY"
#define TS_ADD_DUPLICATE_POLICY_ARG "ON_DUPLICATE"

#define SAMPLES_TO_BYTES(size) (size * sizeof(Sample))

static inline u_int64_t max(u_int64_t a, u_int64_t b) {
    return a > b ? a : b;
}

static inline u_int64_t min(u_int64_t a, u_int64_t b) {
    return a < b ? a : b;
}

#endif
