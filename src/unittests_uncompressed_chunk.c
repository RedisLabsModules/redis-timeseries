/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#include <stdio.h>
#include <stdlib.h>
#include "parse_policies.h"
#include "minunit.h"
#include "compaction.h"
#include "rmutil/alloc.h"
#include "chunk.h"
#include "tsdb.h"


MU_TEST(test_Uncompressed_NewChunk) {
    srand((unsigned int)time(NULL));
    size_t max_chunk_size = 8192;
    for (size_t chunk_size = 2; chunk_size < max_chunk_size; chunk_size+=64 ){
        Chunk *chunk = Uncompressed_NewChunk(chunk_size);
        mu_assert(chunk != NULL, "create uncompressed chunk");
        mu_assert_short_eq(0,chunk->base.numSamples);
        Uncompressed_FreeChunk(chunk);
    }
}

MU_TEST(test_Uncompressed_Uncompressed_AddSample) {
    srand((unsigned int)time(NULL));
    const size_t chunk_size = 4096; // 4096 bytes (data) chunck
     Chunk *chunk = Uncompressed_NewChunk(chunk_size);
    mu_assert(chunk != NULL, "create uncompressed chunk");
    mu_assert_short_eq(0,chunk->base.numSamples);
    ChunkResult rv = CR_OK;
    int64_t ts = 1;
    int64_t total_added_samples = 0;

    // adding 1,3,5....
    while (rv != CR_END){
        double tsv = ts*1.0;
        Sample s1 = { .timestamp = ts, .value = tsv };
        rv = Uncompressed_AddSample(chunk,&s1);
        mu_assert(rv == CR_OK || rv == CR_END, "add sample");
        if(rv!=CR_END){
            total_added_samples++;
            mu_assert_int_eq(total_added_samples,chunk->base.numSamples);
        }
    }
    const size_t chunk_current_size = Uncompressed_GetChunkSize(chunk,false);
    mu_assert_int_eq(chunk_size,chunk_current_size);    
    Uncompressed_FreeChunk(chunk);
}


MU_TEST(test_Uncompressed_Uncompressed_UpsertSample) {
    srand((unsigned int)time(NULL));
    const size_t chunk_size = 4096; // 4096 bytes (data) chunck
     Chunk *chunk = Uncompressed_NewChunk(chunk_size);
    mu_assert(chunk != NULL, "create uncompressed chunk");
    mu_assert_short_eq(0,chunk->base.numSamples);
    ChunkResult rv = CR_OK;
    int64_t ts = 1;
    int64_t total_added_samples = 0;

    // adding 1,3,5....
    while (rv != CR_END){
        double tsv = ts*1.0;
        Sample s1 = { .timestamp = ts, .value = tsv };
        rv = Uncompressed_AddSample(chunk,&s1);
        mu_assert(rv == CR_OK || rv == CR_END, "add sample");
        if(rv!=CR_END){
            total_added_samples++;
            mu_assert_int_eq(total_added_samples,chunk->base.numSamples);
        }
    }
    const size_t chunk_current_size = Uncompressed_GetChunkSize(chunk,false);
    mu_assert_int_eq(chunk_size,chunk_current_size);

    // Now we're at the max of the chunck's capacity
    Sample s3 = { .timestamp = 2, .value =10.0 };
        UpsertCtx uCtxS3 = {
        .inChunk = chunk,
        .sample = s3,
    };

    int size = 0;
    // We're forcing the chunk to grow 
    rv = Uncompressed_UpsertSample(&uCtxS3, &size);
    total_added_samples++;
    mu_assert(rv == CR_OK, "upsert");
    mu_assert_int_eq(total_added_samples,chunk->base.numSamples);
    Uncompressed_FreeChunk(chunk);
}

MU_TEST_SUITE(uncompressed_chunk_test_suite) {
    MU_RUN_TEST(test_Uncompressed_NewChunk);
    MU_RUN_TEST(test_Uncompressed_Uncompressed_AddSample);
    MU_RUN_TEST(test_Uncompressed_Uncompressed_UpsertSample);
}