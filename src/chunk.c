/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "chunk.h"

#include "gears_integration.h"

#include "rmutil/alloc.h"

struct Chunk
{
    timestamp_t base_timestamp;
    unsigned int num_samples;
    size_t size;
    u_int64_t *samples_ts;
    double *samples_values;
};

Chunk_t *Uncompressed_NewChunk(size_t size) {
    Chunk *newChunk = (Chunk *)malloc(sizeof(Chunk));
    newChunk->num_samples = 0;
    newChunk->size = size;
    const size_t array_size = size / 2;
    newChunk->samples_ts = (u_int64_t *)malloc(array_size);
    newChunk->samples_values = (double *)malloc(array_size);
#ifdef DEBUG
    memset(newChunk->samples_ts, 0, array_size);
    memset(newChunk->samples_values, 0, array_size);
#endif

    return newChunk;
}

void Uncompressed_FreeChunk(Chunk_t *chunk) {
    Chunk *curChunk = (Chunk *)chunk;
    free(curChunk->samples_ts);
    free(curChunk->samples_values);
    free(curChunk);
}

/**
 * Split the chunk in half, returning a new chunk with the right-side of the current chunk
 * The input chunk is trimmed to retain the left-most part
 * @param chunk
 * @return new chunk with the right-most splited in half samples
 */
Chunk_t *Uncompressed_SplitChunk(Chunk_t *chunk) {
    Chunk *curChunk = (Chunk *)chunk;
    const size_t newChunkNumSamples = curChunk->num_samples / 2;
    const size_t currentChunkNumSamples = curChunk->num_samples - newChunkNumSamples;

    // create chunk and copy samples
    Chunk *newChunk = Uncompressed_NewChunk(newChunkNumSamples * SAMPLE_SIZE);
    for (size_t i = 0; i < newChunkNumSamples; i++) {
        const u_int64_t ts = curChunk->samples_ts[currentChunkNumSamples + i];
        const double v = curChunk->samples_values[currentChunkNumSamples + i];
        Uncompressed_AddSampleOptimized(newChunk, ts, v);
    }

    // update current chunk
    const size_t old_ts_size = (currentChunkNumSamples * sizeof(u_int64_t));
    const size_t old_values_size = (currentChunkNumSamples * sizeof(double));
    curChunk->num_samples = currentChunkNumSamples;
    curChunk->size = currentChunkNumSamples * SAMPLE_SIZE;
    curChunk->samples_ts = realloc(curChunk->samples_ts, old_ts_size);
    curChunk->samples_values = realloc(curChunk->samples_values, old_values_size);
    return newChunk;
}

static int IsChunkFull(Chunk *chunk) {
    return chunk->num_samples == chunk->size / SAMPLE_SIZE;
}

u_int64_t Uncompressed_NumOfSample(Chunk_t *chunk) {
    return ((Chunk *)chunk)->num_samples;
}

timestamp_t Uncompressed_GetLastTimestamp(Chunk_t *chunk) {
    Chunk *uChunk = (Chunk *)chunk;
    if (uChunk->num_samples == 0) {
        return -1;
    }
    return uChunk->samples_ts[uChunk->num_samples - 1];
}

timestamp_t Uncompressed_GetFirstTimestamp(Chunk_t *chunk) {
    Chunk *uChunk = (Chunk *)chunk;
    if (uChunk->num_samples == 0) {
        return -1;
    }
    return uChunk->samples_ts[0];
}

int Uncompressed_GetSampleValueAtPos(Chunk_t *chunk, size_t pos, double *value) {
    int result = CR_ERR;
    Chunk *uChunk = (Chunk *)chunk;
    if (uChunk->num_samples > pos) {
        *value = uChunk->samples_values[pos];
        result = CR_OK;
    }

    return result;
}
int Uncompressed_GetSampleTimestampAtPos(Chunk_t *chunk, size_t pos, u_int64_t *timestamp) {
    int result = CR_ERR;
    Chunk *uChunk = (Chunk *)chunk;
    if (uChunk->num_samples > pos) {
        *timestamp = uChunk->samples_ts[pos];
        result = CR_OK;
    }
    return result;
}

ChunkResult Uncompressed_AddSampleOptimized(Chunk_t *chunk, u_int64_t timestamp, double value) {
    Chunk *regChunk = (Chunk *)chunk;
    if (IsChunkFull(regChunk)) {
        return CR_END;
    }

    if (regChunk->num_samples == 0) {
        // initialize base_timestamp
        regChunk->base_timestamp = timestamp;
    }
    const size_t pos = regChunk->num_samples;
    regChunk->samples_ts[pos] = timestamp;
    regChunk->samples_values[pos] = value;
    regChunk->num_samples++;
    return CR_OK;
}

ChunkResult Uncompressed_AddSample(Chunk_t *chunk, Sample *sample) {
    return Uncompressed_AddSampleOptimized(chunk, sample->timestamp, sample->value);
}

/**
 * upsertChunk will insert the sample in the chunk no matter the position of insertion.
 * In the case of the chunk being at max capacity we allocate space for one more sample
 * @param chunk
 * @param idx
 * @param sample
 */
static void upsertChunk(Chunk *chunk, size_t idx, u_int64_t ts, double value) {
    if (IsChunkFull(chunk)) {
        chunk->size += SAMPLE_SIZE;
        const size_t new_ts_size = chunk->size / 2 + sizeof(u_int64_t);
        const size_t new_values_size = chunk->size / 2 + sizeof(double);
        chunk->samples_ts = realloc(chunk->samples_ts, new_ts_size);
        chunk->samples_values = realloc(chunk->samples_values, new_values_size);
    }
    if (idx < chunk->num_samples) { // sample is not last
        memmove(&chunk->samples_ts[idx + 1],
                &chunk->samples_ts[idx],
                (chunk->num_samples - idx) * sizeof(u_int64_t));
        memmove(&chunk->samples_values[idx + 1],
                &chunk->samples_values[idx],
                (chunk->num_samples - idx) * sizeof(double));
    }
    chunk->samples_ts[idx] = ts;
    chunk->samples_values[idx] = value;
    chunk->num_samples++;
}

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult Uncompressed_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy) {
    *size = 0;
    Chunk *regChunk = (Chunk *)uCtx->inChunk;
    const u_int64_t ts = uCtx->sample.timestamp;
    const u_int64_t *ts_array = regChunk->samples_ts;
    const size_t numSamples = regChunk->num_samples;
    size_t sample_pos = 0;
    u_int64_t sample_ts = 0;
    bool found = false;

    // find the number of elements in the array that are less than the timestamp you search for
    for (int i = 0; i < numSamples; i++)
        sample_pos += (ts_array[i] < ts);

    // check if timestamp right after is the one we're searching for
    if (sample_pos < numSamples && ts_array[sample_pos] == ts)
        found = true;

    // update value in case timestamp exists
    if (found == true) {
        ChunkResult cr = handleDuplicateSample(
            duplicatePolicy, regChunk->samples_values[sample_pos], &(uCtx->sample.value));
        if (cr != CR_OK) {
            return CR_ERR;
        }
        regChunk->samples_values[sample_pos] = uCtx->sample.value;
        return CR_OK;
    }

    if (sample_pos == 0) {
        regChunk->base_timestamp = ts;
    }

    upsertChunk(regChunk, sample_pos, ts, uCtx->sample.value);
    *size = 1;
    return CR_OK;
}

size_t Uncompressed_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs) {
    Chunk *regChunk = (Chunk *)chunk;
    const u_int64_t *timestamps = regChunk->samples_ts;
    const double *values = regChunk->samples_values;

    // create two new arrays and copy samples that don't match the delete range
    // TODO: use memove that should be much faster
    const size_t array_size = regChunk->size / 2;
    u_int64_t *new_samples_ts = (u_int64_t *)malloc(array_size);
    double *new_samples_values = (double *)malloc(array_size);
    size_t i = 0;
    size_t new_count = 0;
    for (; i < regChunk->num_samples; ++i) {
        if (timestamps[i] >= startTs && timestamps[i] <= endTs) {
            continue;
        }
        new_samples_ts[new_count] = timestamps[i];
        new_samples_values[new_count] = values[i];
        new_count++;
    }
    size_t deleted_count = regChunk->num_samples - new_count;
    free(regChunk->samples_ts);
    free(regChunk->samples_values);
    regChunk->samples_ts = new_samples_ts;
    regChunk->samples_values = new_samples_values;
    regChunk->num_samples = new_count;
    regChunk->base_timestamp = new_samples_ts[0];
    return deleted_count;
}

void Uncompressed_ResetChunkIterator(ChunkIter_t *iterator, Chunk_t *chunk) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    iter->chunk = chunk;
    if (iter->options & CHUNK_ITER_OP_REVERSE) { // iterate from last to first
        iter->currentIndex = iter->chunk->num_samples - 1;
    } else { // iterate from first to last
        iter->currentIndex = 0;
    }
}

ChunkIter_t *Uncompressed_NewChunkIterator(Chunk_t *chunk,
                                           int options,
                                           ChunkIterFuncs *retChunkIterClass) {
    ChunkIterator *iter = (ChunkIterator *)calloc(1, sizeof(ChunkIterator));
    iter->options = options;
    if (retChunkIterClass != NULL) {
        *retChunkIterClass = *GetChunkIteratorClass(CHUNK_REGULAR);
    }
    Uncompressed_ResetChunkIterator(iter, chunk);
    return (ChunkIter_t *)iter;
}

ChunkResult Uncompressed_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->currentIndex < iter->chunk->num_samples) {
        sample->value = iter->chunk->samples_values[iter->currentIndex];
        sample->timestamp = iter->chunk->samples_ts[iter->currentIndex];
        iter->currentIndex++;
        return CR_OK;
    } else {
        return CR_END;
    }
}

ChunkResult Uncompressed_ChunkIteratorGetPrev(ChunkIter_t *iterator, Sample *sample) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->currentIndex >= 0) {
        sample->value = iter->chunk->samples_values[iter->currentIndex];
        sample->timestamp = iter->chunk->samples_ts[iter->currentIndex];
        iter->currentIndex--;
        return CR_OK;
    } else {
        return CR_END;
    }
}

void Uncompressed_FreeChunkIterator(ChunkIter_t *iterator) {
    ChunkIterator *iter = (ChunkIterator *)iterator;
    if (iter->options & CHUNK_ITER_OP_FREE_CHUNK) {
        Uncompressed_FreeChunk(iter->chunk);
    }
    free(iter);
}

size_t Uncompressed_GetChunkSize(Chunk_t *chunk, bool includeStruct) {
    Chunk *uncompChunk = chunk;
    size_t size = uncompChunk->size;
    size += includeStruct ? sizeof(*uncompChunk) : 0;
    return size;
}

typedef void (*SaveUnsignedFunc)(void *, uint64_t);
typedef void (*SaveStringBufferFunc)(void *, const char *str, size_t len);
typedef uint64_t (*ReadUnsignedFunc)(void *);
typedef char *(*ReadStringBufferFunc)(void *, size_t *);

static void Uncompressed_GenericSerialize(Chunk_t *chunk,
                                          void *ctx,
                                          SaveUnsignedFunc saveUnsigned,
                                          SaveStringBufferFunc saveString) {
    Chunk *uncompchunk = chunk;

    saveUnsigned(ctx, uncompchunk->base_timestamp);
    saveUnsigned(ctx, uncompchunk->num_samples);
    saveUnsigned(ctx, uncompchunk->size);
    Sample *old_samples_array = (Sample *)malloc(uncompchunk->size * sizeof(Sample));
    for (size_t i = 0; i < uncompchunk->num_samples; i++) {
        old_samples_array[i].timestamp = uncompchunk->samples_ts[i];
        old_samples_array[i].value = uncompchunk->samples_values[i];
    }
    saveString(ctx, (char *)old_samples_array, uncompchunk->size);
}

static void Uncompressed_Deserialize(Chunk_t **chunk,
                                     void *ctx,
                                     ReadUnsignedFunc readUnsigned,
                                     ReadStringBufferFunc readStringBuffer) {
    const timestamp_t base_timestamp = readUnsigned(ctx);
    const unsigned int num_samples = readUnsigned(ctx);
    const size_t size = readUnsigned(ctx);
    Chunk *uncompchunk = Uncompressed_NewChunk(size);
    uncompchunk->base_timestamp = base_timestamp;
    uncompchunk->num_samples = num_samples;

    size_t loadsize;
    Sample *old_samples_array = (Sample *)readStringBuffer(ctx, &loadsize);
    for (size_t i = 0; i < uncompchunk->num_samples; i++) {
        uncompchunk->samples_ts[i] = old_samples_array[i].timestamp;
        uncompchunk->samples_values[i] = old_samples_array[i].value;
    }
    *chunk = (Chunk_t *)uncompchunk;
}

void Uncompressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io) {
    Uncompressed_GenericSerialize(chunk,
                                  io,
                                  (SaveUnsignedFunc)RedisModule_SaveUnsigned,
                                  (SaveStringBufferFunc)RedisModule_SaveStringBuffer);
}

void Uncompressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io) {
    Uncompressed_Deserialize(chunk,
                             io,
                             (ReadUnsignedFunc)RedisModule_LoadUnsigned,
                             (ReadStringBufferFunc)RedisModule_LoadStringBuffer);
}

void Uncompressed_GearsSerialize(Chunk_t *chunk, Gears_BufferWriter *bw) {}

void Uncompressed_GearsDeserialize(Chunk_t *chunk, Gears_BufferReader *br) {}
