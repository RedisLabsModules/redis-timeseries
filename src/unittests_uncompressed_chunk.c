/*
 * Copyright 2018-2020 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "chunk.h"
#include "compaction.h"
#include "minunit.h"
#include "parse_policies.h"
#include "tsdb.h"

#include <stdio.h>
#include <stdlib.h>
#include "rmutil/alloc.h"

MU_TEST(test_Uncompressed_NewChunk) {
    srand((unsigned int)time(NULL));
    size_t max_chunk_size = 8192;
    for (size_t chunk_size = 2; chunk_size < max_chunk_size; chunk_size += 64) {
        Chunk *chunk = Uncompressed_NewChunk(false, chunk_size);
        mu_assert(chunk != NULL, "create uncompressed chunk");
        mu_assert_short_eq(0, chunk->num_samples);
        Uncompressed_FreeChunk(chunk);
    }
}

MU_TEST(test_Uncompressed_Uncompressed_AddSample) {
    srand((unsigned int)time(NULL));
    const size_t chunk_size = 4096; // 4096 bytes (data) chunck
    Chunk *chunk = Uncompressed_NewChunk(false, chunk_size);
    mu_assert(chunk != NULL, "create uncompressed chunk");
    mu_assert_short_eq(0, chunk->num_samples);
    ChunkResult rv = CR_OK;
    int64_t ts = 1;
    int64_t total_added_samples = 0;

    // adding 1,3,5....
    while (rv != CR_END) {
        double tsv = ts * 1.0;
        Sample s1 = { .timestamp = ts, .value.d.value = tsv };
        rv = Uncompressed_AddSample(chunk, &s1);
        mu_assert(rv == CR_OK || rv == CR_END, "add sample");
        if (rv != CR_END) {
            total_added_samples++;
            mu_assert_int_eq(total_added_samples, chunk->num_samples);
        }
    }
    const size_t chunk_current_size = Uncompressed_GetChunkSize(chunk, false);
    mu_assert_int_eq(chunk_size, chunk_current_size);
    Uncompressed_FreeChunk(chunk);
}

MU_TEST(test_Uncompressed_Uncompressed_UpsertSample) {
    srand((unsigned int)time(NULL));
    const size_t chunk_size = 4096; // 4096 bytes (data) chunck
    Chunk *chunk = Uncompressed_NewChunk(false, chunk_size);
    mu_assert(chunk != NULL, "create uncompressed chunk");
    mu_assert_short_eq(0, chunk->num_samples);
    ChunkResult rv = CR_OK;
    int64_t ts = 1;
    int64_t total_added_samples = 0;

    // adding 1,3,5....
    while (rv != CR_END) {
        double tsv = ts * 1.0;
        Sample s1 = { .timestamp = ts, .value.d.value = tsv };
        rv = Uncompressed_AddSample(chunk, &s1);
        mu_assert(rv == CR_OK || rv == CR_END, "add sample");
        if (rv != CR_END) {
            total_added_samples++;
            mu_assert_int_eq(total_added_samples, chunk->num_samples);
        }
    }
    const size_t chunk_current_size = Uncompressed_GetChunkSize(chunk, false);
    mu_assert_int_eq(chunk_size, chunk_current_size);

    // Now we're at the max of the chunck's capacity
    Sample s3 = { .timestamp = 2, .value.d.value = 10.0 };
    UpsertCtx uCtxS3 = {
        .inChunk = chunk,
        .sample = s3,
    };

    int size = 0;
    // We're forcing the chunk to grow
    rv = Uncompressed_UpsertSample(&uCtxS3, &size, DP_LAST);
    total_added_samples++;
    mu_assert(rv == CR_OK, "upsert");
    mu_assert_int_eq(total_added_samples, chunk->num_samples);
    Uncompressed_FreeChunk(chunk);
}

MU_TEST(test_Uncompressed_Uncompressed_UpsertSample_DuplicatePolicy) {
    srand((unsigned int)time(NULL));
    const size_t chunk_size = 4096; // 4096 bytes (data) chunck
    Chunk *chunk = Uncompressed_NewChunk(false, chunk_size);
    mu_assert(chunk != NULL, "create uncompressed chunk");
    mu_assert_short_eq(0, chunk->num_samples);
    ChunkResult rv = CR_OK;
    Sample s1 = { .timestamp = 1, .value.d.value = -0.5 };
    Sample s2 = { .timestamp = 1, .value.d.value = -0.6 };
    rv = Uncompressed_AddSample(chunk, &s1);
    mu_assert(rv == CR_OK, "add sample");
    UpsertCtx uCtx = {
        .inChunk = chunk,
        .sample = s2,
    };

    int size = 0;
    // We're forcing the chunk to insert a duplicate and test different policies
    // DP_BLOCK should not change old sample
    rv = Uncompressed_UpsertSample(&uCtx, &size, DP_BLOCK);
    mu_assert(rv == CR_ERR, "duplicate block");
    mu_assert_int_eq(1, chunk->num_samples);
    const u_int64_t firstTs = Uncompressed_GetFirstTimestamp(chunk);
    mu_assert_int_eq(1, firstTs);
    mu_assert_double_eq(-0.5, VALUE_DOUBLE(&chunk->samples[0].value));
    // DP_MAX should keep -0.5 given that -0.4 is smaller
    VALUE_DOUBLE(&uCtx.sample.value) = -0.4;
    rv = Uncompressed_UpsertSample(&uCtx, &size, DP_MIN);
    mu_assert(rv == CR_OK, "duplicate min not changing old value");
    mu_assert_int_eq(1, chunk->num_samples);
    mu_assert_double_eq(-0.5, VALUE_DOUBLE(&chunk->samples[0].value));
    // DP_MIN should replace -0.5 by -0.6
    VALUE_DOUBLE(&uCtx.sample.value) = -0.6;
    rv = Uncompressed_UpsertSample(&uCtx, &size, DP_MIN);
    mu_assert(rv == CR_OK, "duplicate min changing old value");
    mu_assert_int_eq(1, chunk->num_samples);
    mu_assert_double_eq(-0.6, VALUE_DOUBLE(&chunk->samples[0].value));
    // DP_MAX should keep -0.6 given that -1 is smaller
    VALUE_DOUBLE(&uCtx.sample.value) = -1.0;
    rv = Uncompressed_UpsertSample(&uCtx, &size, DP_MAX);
    mu_assert(rv == CR_OK, "duplicate max not changing old value");
    mu_assert_double_eq(-0.6, VALUE_DOUBLE(&chunk->samples[0].value));
    // DP_MAX should replace -0.6 by -0.2
    VALUE_DOUBLE(&uCtx.sample.value) = -0.2;
    rv = Uncompressed_UpsertSample(&uCtx, &size, DP_MAX);
    mu_assert(rv == CR_OK, "duplicate max changing old value");
    mu_assert_double_eq(-0.2, VALUE_DOUBLE(&chunk->samples[0].value));
    Uncompressed_FreeChunk(chunk);
}

MU_TEST_SUITE(uncompressed_chunk_test_suite) {
    MU_RUN_TEST(test_Uncompressed_NewChunk);
    MU_RUN_TEST(test_Uncompressed_Uncompressed_AddSample);
    MU_RUN_TEST(test_Uncompressed_Uncompressed_UpsertSample);
    MU_RUN_TEST(test_Uncompressed_Uncompressed_UpsertSample_DuplicatePolicy);
}
