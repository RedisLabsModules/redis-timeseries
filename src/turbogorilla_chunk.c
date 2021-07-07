/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "turbogorilla_chunk.h"

#include "../../../../../usr/include/stdlib.h"
#include "../../../../../usr/include/string.h"
#include "assert.h"
#include "compressed_chunk.h"
#include "fp.h"
#include "gears_integration.h"

// Overflow guard size
#define FP_USIZE 64

#include "rmutil/alloc.h"

struct TurboGorilla_Chunk
{
    timestamp_t start_ts;
    timestamp_t end_ts;
    unsigned int num_samples;
    size_t size;
    bool buffer_in_use;
    u_int64_t *buffer_ts;
    double *buffer_values;
    size_t compressed_ts_size;
    size_t compressed_values_size;
    unsigned char *compressed_ts;
    unsigned char *compressed_values;
};

struct TurboGorilla_ChunkIterator
{
    int currentIndex;
    timestamp_t lastTimestamp;
    int lastValue;
    int options;
    TurboGorilla_Chunk *chunk;
    uint64_t *decompressed_ts;
    double *decompressed_values;
};

void _TG_decompress_to_buffer(TurboGorilla_Chunk *g_chunk,
                              uint64_t *buffer_ts,
                              double *buffer_values);
void _TG_compress_from_buffer(TurboGorilla_Chunk *regChunk);
void _TG_alloc_buffer(size_t size, uint64_t **buffer_ts, double **buffer_values);
void _TG_alloc_compressed(size_t size, TurboGorilla_Chunk *newChunk);
void _TG_free_buffer(const TurboGorilla_Chunk *g_chunk);
void _TG_free_compressed(const TurboGorilla_Chunk *g_chunk);
static void _TG_expand_buffer(TurboGorilla_Chunk *chunk, size_t a);
static void _TG_shift_on_index(const TurboGorilla_Chunk *chunk, size_t idx);

void reset(void *chunk,
           int options,
           ChunkIterFuncs *retChunkIterClass,
           TurboGorilla_ChunkIterator *iter);

Chunk_t *TurboGorilla_NewChunk(size_t size) {
    TurboGorilla_Chunk *newChunk = (TurboGorilla_Chunk *)malloc(sizeof(TurboGorilla_Chunk));
    newChunk->num_samples = 0;
    newChunk->size = size;
    newChunk->start_ts = 0;
    newChunk->end_ts = 0;
    _TG_alloc_buffer(size, &(newChunk->buffer_ts), &(newChunk->buffer_values));
    newChunk->buffer_in_use = true;
    return newChunk;
}

void _TG_alloc_buffer(size_t size, uint64_t **buffer_ts, double **buffer_values) {
    const size_t array_size = size / 2;
    *buffer_ts = (u_int64_t *)malloc(array_size);
    *buffer_values = (double *)malloc(array_size);
#ifdef DEBUG
    memset(*buffer_ts, 0, array_size);
    memset(*buffer_values, 0, array_size);
#endif
}

void _TG_alloc_compressed(size_t size, TurboGorilla_Chunk *newChunk) {
    const size_t array_size = size / 2;
    newChunk->compressed_ts = (unsigned char *)malloc(array_size);
    newChunk->compressed_values = (unsigned char *)malloc(array_size);
}

void TurboGorilla_FreeChunk(Chunk_t *chunk) {
    TurboGorilla_Chunk *curChunk = (TurboGorilla_Chunk *)chunk;
    if (curChunk->buffer_in_use) {
        _TG_free_buffer(curChunk);
    } else {
        _TG_free_compressed(curChunk);
    }
    free(curChunk);
}

Chunk_t *TurboGorilla_CloneChunk(Chunk_t *chunk) {
    TurboGorilla_Chunk *curChunk = (TurboGorilla_Chunk *)chunk;
    TurboGorilla_Chunk *newChunk = TurboGorilla_NewChunk(curChunk->size);
    if (curChunk->buffer_in_use) {
        memcpy(newChunk->buffer_ts, curChunk->buffer_ts, curChunk->num_samples * sizeof(uint64_t));
        memcpy(newChunk->buffer_values,
               curChunk->buffer_values,
               curChunk->num_samples * sizeof(double));

    } else {
        _TG_free_buffer(newChunk);
        _TG_alloc_compressed(curChunk->size, newChunk);
        memcpy(newChunk->compressed_ts, curChunk->compressed_ts, curChunk->compressed_ts_size);
        memcpy(newChunk->compressed_values,
               curChunk->compressed_values,
               curChunk->compressed_values_size);
    }
    newChunk->num_samples = curChunk->num_samples;
    newChunk->size = curChunk->size;
    newChunk->start_ts = curChunk->start_ts;
    newChunk->end_ts = curChunk->end_ts;
    newChunk->buffer_in_use = curChunk->buffer_in_use;
    newChunk->compressed_values_size = curChunk->compressed_values_size;
    newChunk->compressed_ts_size = curChunk->compressed_ts_size;
    return newChunk;
}

/**
 * Split the chunk in half, returning a new chunk with the right-side of the current chunk
 * The input chunk is trimmed to retain the left-most part
 * @param chunk
 * @return new chunk with the right-most splited in half samples
 */
Chunk_t *TurboGorilla_SplitChunk(Chunk_t *chunk) {
    TurboGorilla_Chunk *curChunk = (TurboGorilla_Chunk *)chunk;
    const size_t newChunkNumSamples = curChunk->num_samples / 2;
    const size_t currentChunkNumSamples = curChunk->num_samples - newChunkNumSamples;

    // create chunk and copy samples
    TurboGorilla_Chunk *newChunk =
        TurboGorilla_NewChunk(newChunkNumSamples * TURBOGORILLA_SAMPLE_SIZE);
    for (size_t i = 0; i < newChunkNumSamples; i++) {
        const u_int64_t ts = curChunk->buffer_ts[currentChunkNumSamples + i];
        const double v = curChunk->buffer_values[currentChunkNumSamples + i];
        TurboGorilla_AddSampleOptimized(newChunk, ts, v);
    }

    // update current chunk
    const size_t old_ts_size = (currentChunkNumSamples * sizeof(u_int64_t));
    const size_t old_values_size = (currentChunkNumSamples * sizeof(double));
    curChunk->num_samples = currentChunkNumSamples;
    curChunk->size = currentChunkNumSamples * TURBOGORILLA_SAMPLE_SIZE;
    curChunk->buffer_ts = realloc(curChunk->buffer_ts, old_ts_size);
    curChunk->buffer_values = realloc(curChunk->buffer_values, old_values_size);
    return newChunk;
}

static int TurboGorilla_IsChunkFull(TurboGorilla_Chunk *chunk) {
    return chunk->num_samples >= ((chunk->size / TURBOGORILLA_SAMPLE_SIZE) - FP_USIZE);
}

u_int64_t TurboGorilla_NumOfSample(Chunk_t *chunk) {
    return ((TurboGorilla_Chunk *)chunk)->num_samples;
}

timestamp_t TurboGorilla_GetLastTimestamp(Chunk_t *chunk) {
    TurboGorilla_Chunk *uChunk = (TurboGorilla_Chunk *)chunk;
    if (uChunk->num_samples == 0) {
        return -1;
    }
    return uChunk->end_ts;
}

timestamp_t TurboGorilla_GetFirstTimestamp(Chunk_t *chunk) {
    TurboGorilla_Chunk *uChunk = (TurboGorilla_Chunk *)chunk;
    if (uChunk->num_samples == 0) {
        return -1;
    }
    return uChunk->start_ts;
}

ChunkResult TurboGorilla_AddSampleOptimized(Chunk_t *chunk, u_int64_t timestamp, double value) {
    TurboGorilla_Chunk *regChunk = (TurboGorilla_Chunk *)chunk;
    if (TurboGorilla_IsChunkFull(regChunk)) {
        return CR_END;
    }
    // initialize start_ts
    if (regChunk->num_samples == 0) {
        regChunk->start_ts = timestamp;
    }
    regChunk->end_ts = timestamp;
    const size_t pos = regChunk->num_samples;
    regChunk->buffer_ts[pos] = timestamp;
    regChunk->buffer_values[pos] = value;
    regChunk->num_samples++;

    if (TurboGorilla_IsChunkFull(regChunk)) {
        _TG_alloc_compressed(regChunk->size, regChunk);
        _TG_compress_from_buffer(regChunk);
        _TG_free_buffer(regChunk);
        regChunk->buffer_in_use = false;
    }
    return CR_OK;
}

// TurboGorilla : Improved gorilla style + RLE (bit/io)
// Decompress from TurboGorilla to SoA
void _TG_decompress_to_buffer(TurboGorilla_Chunk *g_chunk,
                              uint64_t *buffer_ts,
                              double *buffer_values) {
    /* decoding functions are of the form:
     * void decode(char *out, size_t n, unsigned *in, unsigned start);
     *    - in : pointer to input buffer
     *    - n : number of elements
     *    - out : output array
     *    - start : previous value. Only for integrated delta encoding functions
     */
    fpgdec64(g_chunk->compressed_ts, g_chunk->num_samples, buffer_ts, 0);
    fpgdec64(g_chunk->compressed_values, g_chunk->num_samples, buffer_values, 0);
}

void _TG_compress_from_buffer(TurboGorilla_Chunk *g_chunk) {
    /* encoding functions are of the form:
     * size_t compressed_size = encode( unsigned *in, size_t n, char *out, unsigned start);
     *    - compressed_size : number of bytes written into compressed output buffer out
     *    - in : input array
     *    - n : number of elements
     *    - out : pointer to output buffer
     *    - start : previous value. Only for integrated delta encoding functions
     */
    g_chunk->compressed_ts_size =
        fpgenc64(g_chunk->buffer_ts, g_chunk->num_samples, g_chunk->compressed_ts, 0);
#ifdef DEBUG
    assert(g_chunk->compressed_ts_size <= (g_chunk->size / 2));
#endif
    g_chunk->compressed_values_size =
        fpgenc64(g_chunk->buffer_values, g_chunk->num_samples, g_chunk->compressed_values, 0);
#ifdef DEBUG
    assert(g_chunk->compressed_values_size <= (g_chunk->size / 2));
#endif
}

void _TG_free_compressed(const TurboGorilla_Chunk *g_chunk) {
    free(g_chunk->compressed_ts);
    free(g_chunk->compressed_values);
}

void _TG_free_buffer(const TurboGorilla_Chunk *g_chunk) {
    free(g_chunk->buffer_ts);
    free(g_chunk->buffer_values);
}

ChunkResult TurboGorilla_AddSample(Chunk_t *chunk, Sample *sample) {
    return TurboGorilla_AddSampleOptimized(chunk, sample->timestamp, sample->value);
}

/**
 * upsertTurboGorilla_Chunk will insert the sample in the chunk no matter the position of insertion.
 * In the case of the chunk being at max capacity we allocate space for one more sample
 * @param chunk
 * @param idx
 * @param sample
 */
static void upsertChunk(TurboGorilla_Chunk *chunk, size_t idx, u_int64_t ts, double value) {
    if (TurboGorilla_IsChunkFull(chunk)) {
        _TG_expand_buffer(chunk, TURBOGORILLA_SAMPLE_SIZE);
    }
    if (idx < chunk->num_samples) { // sample is not last
        _TG_shift_on_index(chunk, idx);
    }
    chunk->buffer_ts[idx] = ts;
    chunk->buffer_values[idx] = value;
    chunk->num_samples++;
}

static void _TG_shift_on_index(const TurboGorilla_Chunk *chunk, size_t idx) {
    memmove(&chunk->buffer_ts[idx + 1],
            &chunk->buffer_ts[idx],
            (chunk->num_samples - idx) * sizeof(u_int64_t));
    memmove(&chunk->buffer_values[idx + 1],
            &chunk->buffer_values[idx],
            (chunk->num_samples - idx) * sizeof(double));
}

static void _TG_expand_buffer(TurboGorilla_Chunk *chunk, size_t increase_by_bytes) {
    chunk->size += increase_by_bytes;
    const size_t new_ts_size = chunk->size / 2 + sizeof(u_int64_t);
    const size_t new_values_size = chunk->size / 2 + sizeof(double);
    chunk->buffer_ts = realloc(chunk->buffer_ts, new_ts_size);
    chunk->buffer_values = realloc(chunk->buffer_values, new_values_size);
}

/**
 * TODO: describe me
 * @param uCtx
 * @param size
 * @return
 */
ChunkResult TurboGorilla_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy) {
    *size = 0;
    TurboGorilla_Chunk *regChunk = (TurboGorilla_Chunk *)uCtx->inChunk;
    const u_int64_t ts = uCtx->sample.timestamp;
    // If we're using the compressed version, decompress it
    const bool was_compressed = regChunk->buffer_in_use == false;
    if (was_compressed) {
        _TG_alloc_buffer(regChunk->size, &(regChunk->buffer_ts), &(regChunk->buffer_values));
        _TG_decompress_to_buffer(regChunk, regChunk->buffer_ts, regChunk->buffer_values);
    }
    const u_int64_t *ts_array = regChunk->buffer_ts;
    const size_t numSamples = regChunk->num_samples;
    size_t sample_pos = 0;
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
            duplicatePolicy, regChunk->buffer_values[sample_pos], &(uCtx->sample.value));
        if (cr != CR_OK) {
            if (was_compressed) {
                _TG_compress_from_buffer(regChunk);
                _TG_free_buffer(regChunk);
            }
            return CR_ERR;
        }
        regChunk->buffer_values[sample_pos] = uCtx->sample.value;
        if (was_compressed) {
            _TG_compress_from_buffer(regChunk);
            _TG_free_buffer(regChunk);
        }
        return CR_OK;
    }

    if (sample_pos == 0) {
        regChunk->start_ts = ts;
    }

    upsertChunk(regChunk, sample_pos, ts, uCtx->sample.value);
    if (was_compressed) {
        _TG_compress_from_buffer(regChunk);
        _TG_free_buffer(regChunk);
    }
    *size = 1;
    return CR_OK;
}

size_t TurboGorilla_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs) {
    TurboGorilla_Chunk *regChunk = (TurboGorilla_Chunk *)chunk;
    const bool was_compressed = regChunk->buffer_in_use == false;
    if (was_compressed) {
        _TG_alloc_buffer(regChunk->size, &(regChunk->buffer_ts), &(regChunk->buffer_values));
        _TG_decompress_to_buffer(regChunk, regChunk->buffer_ts, regChunk->buffer_values);
    }
    const u_int64_t *timestamps = regChunk->buffer_ts;
    const double *values = regChunk->buffer_values;

    // create two new arrays and copy samples that don't match the delete range
    // TODO: use memove that should be much faster
    const size_t array_size = regChunk->size / 2;
    u_int64_t *new_buffer_ts = (u_int64_t *)malloc(array_size);
    double *new_buffer_values = (double *)malloc(array_size);
    size_t i = 0;
    size_t new_count = 0;
    for (; i < regChunk->num_samples; ++i) {
        if (timestamps[i] >= startTs && timestamps[i] <= endTs) {
            continue;
        }
        new_buffer_ts[new_count] = timestamps[i];
        new_buffer_values[new_count] = values[i];
        new_count++;
    }
    size_t deleted_count = regChunk->num_samples - new_count;
    _TG_free_buffer(regChunk);
    regChunk->buffer_ts = new_buffer_ts;
    regChunk->buffer_values = new_buffer_values;
    regChunk->num_samples = new_count;
    regChunk->start_ts = new_buffer_ts[0];
    regChunk->end_ts = new_buffer_ts[new_count];
    if (was_compressed) {
        _TG_compress_from_buffer(regChunk);
        _TG_free_buffer(regChunk);
    }
    return deleted_count;
}

ChunkIter_t *TurboGorilla_NewChunkIterator(Chunk_t *chunk,
                                           int options,
                                           ChunkIterFuncs *retChunkIterClass) {
    TurboGorilla_ChunkIterator *iter =
        (TurboGorilla_ChunkIterator *)calloc(1, sizeof(TurboGorilla_ChunkIterator));
    TurboGorilla_Chunk *compressedChunk = (TurboGorilla_Chunk *)chunk;
    _TG_alloc_buffer(compressedChunk->size, &(iter->decompressed_ts), &(iter->decompressed_values));
    iter->options = options;
    if (retChunkIterClass != NULL) {
        *retChunkIterClass = *GetChunkIteratorClass(CHUNK_COMPRESSED_TURBOGORILLA);
    }
    TurboGorilla_ResetChunkIterator(iter, chunk);
    return (ChunkIter_t *)iter;
}

void TurboGorilla_ResetChunkIterator(ChunkIter_t *iterator, Chunk_t *chunk) {
    TurboGorilla_Chunk *compressedChunk = (TurboGorilla_Chunk *)chunk;
    TurboGorilla_ChunkIterator *iter = (TurboGorilla_ChunkIterator *)iterator;
    iter->chunk = compressedChunk;
    if (iter->options & CHUNK_ITER_OP_REVERSE) { // iterate from last to first
        iter->currentIndex = compressedChunk->num_samples - 1;
    } else { // iterate from first to last
        iter->currentIndex = 0;
    }
    if (compressedChunk->num_samples > 0) {
        if (compressedChunk->buffer_in_use == false) {
            _TG_decompress_to_buffer(
                compressedChunk, iter->decompressed_ts, iter->decompressed_values);
        } else {
            memcpy(iter->decompressed_ts,
                   compressedChunk->buffer_ts,
                   compressedChunk->num_samples * sizeof(uint64_t));
            memcpy(iter->decompressed_values,
                   compressedChunk->buffer_values,
                   compressedChunk->num_samples * sizeof(double));
        }
    }
}

ChunkResult TurboGorilla_ChunkIteratorGetNext(ChunkIter_t *iterator, Sample *sample) {
    TurboGorilla_ChunkIterator *iter = (TurboGorilla_ChunkIterator *)iterator;
    if (iter->currentIndex < iter->chunk->num_samples) {
        sample->value = iter->decompressed_values[iter->currentIndex];
        sample->timestamp = iter->decompressed_ts[iter->currentIndex];
        iter->currentIndex++;
        return CR_OK;
    } else {
        return CR_END;
    }
}

ChunkResult TurboGorilla_ChunkIteratorGetPrev(ChunkIter_t *iterator, Sample *sample) {
    TurboGorilla_ChunkIterator *iter = (TurboGorilla_ChunkIterator *)iterator;
    if (iter->currentIndex >= 0 && iter->chunk->num_samples > 0) {
        sample->value = iter->decompressed_values[iter->currentIndex];
        sample->timestamp = iter->decompressed_ts[iter->currentIndex];
        iter->currentIndex--;
        return CR_OK;
    } else {
        return CR_END;
    }
}

void TurboGorilla_FreeChunkIterator(ChunkIter_t *iterator) {
    TurboGorilla_ChunkIterator *iter = (TurboGorilla_ChunkIterator *)iterator;
    free(iter->decompressed_ts);
    free(iter->decompressed_values);
    free(iter);
}

size_t TurboGorilla_GetChunkSize(Chunk_t *chunk, bool includeStruct) {
    TurboGorilla_Chunk *uncompChunk = (TurboGorilla_Chunk *)chunk;
    size_t size = uncompChunk->buffer_in_use
                      ? uncompChunk->size
                      : (uncompChunk->compressed_ts_size + uncompChunk->compressed_values_size);
    size += includeStruct ? sizeof(*uncompChunk) : 0;
    return size;
}

typedef void (*SaveUnsignedFunc)(void *, uint64_t);
typedef void (*SaveStringBufferFunc)(void *, const char *str, size_t len);
typedef uint64_t (*ReadUnsignedFunc)(void *);
typedef char *(*ReadStringBufferFunc)(void *, size_t *);

static void TurboGorilla_GenericSerialize(Chunk_t *chunk,
                                          void *ctx,
                                          SaveUnsignedFunc saveUnsigned,
                                          SaveStringBufferFunc saveString) {
    TurboGorilla_Chunk *uncompchunk = (TurboGorilla_Chunk *)chunk;
    if (uncompchunk) {
        saveUnsigned(ctx, uncompchunk->size);
        saveUnsigned(ctx, uncompchunk->start_ts);
        saveUnsigned(ctx, uncompchunk->end_ts);
        saveUnsigned(ctx, uncompchunk->num_samples);
        saveUnsigned(ctx, uncompchunk->buffer_in_use);
        if (uncompchunk->buffer_in_use) {
            _TG_alloc_compressed(uncompchunk->size, uncompchunk);
            _TG_compress_from_buffer(uncompchunk);
        }
        if (uncompchunk->num_samples > 0) {
            saveString(ctx, (char *)uncompchunk->compressed_ts, uncompchunk->compressed_ts_size);
            saveString(
                ctx, (char *)uncompchunk->compressed_values, uncompchunk->compressed_values_size);
        }
    }
}

static void TurboGorilla_Deserialize(Chunk_t **chunk,
                                     void *ctx,
                                     ReadUnsignedFunc readUnsigned,
                                     ReadStringBufferFunc readStringBuffer) {
    const size_t size = readUnsigned(ctx);
    TurboGorilla_Chunk *uncompchunk = TurboGorilla_NewChunk(size);
    uncompchunk->start_ts = readUnsigned(ctx);
    uncompchunk->end_ts = readUnsigned(ctx);
    uncompchunk->num_samples = readUnsigned(ctx);
    uncompchunk->buffer_in_use = readUnsigned(ctx);
    if (uncompchunk->num_samples > 0) {
        uncompchunk->compressed_ts =
            (uint64_t *)readStringBuffer(ctx, &uncompchunk->compressed_ts_size);
        uncompchunk->compressed_values =
            (double *)readStringBuffer(ctx, &uncompchunk->compressed_values_size);
        if (uncompchunk->buffer_in_use) {
            _TG_alloc_buffer(
                uncompchunk->size, &(uncompchunk->buffer_ts), &(uncompchunk->buffer_values));
            _TG_decompress_to_buffer(
                uncompchunk, uncompchunk->buffer_ts, uncompchunk->buffer_values);
            _TG_free_compressed(uncompchunk);
        }
    }
    *chunk = (Chunk_t *)uncompchunk;
}

void TurboGorilla_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io) {
    TurboGorilla_GenericSerialize(chunk,
                                  io,
                                  (SaveUnsignedFunc)RedisModule_SaveUnsigned,
                                  (SaveStringBufferFunc)RedisModule_SaveStringBuffer);
}

void TurboGorilla_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io) {
    TurboGorilla_Deserialize(chunk,
                             io,
                             (ReadUnsignedFunc)RedisModule_LoadUnsigned,
                             (ReadStringBufferFunc)RedisModule_LoadStringBuffer);
}

void TurboGorilla_GearsSerialize(Chunk_t *chunk, Gears_BufferWriter *bw) {
    TurboGorilla_GenericSerialize(chunk,
                                  bw,
                                  (SaveUnsignedFunc)RedisGears_BWWriteLong,
                                  (SaveStringBufferFunc)RedisGears_BWWriteBuffer);
}

void TurboGorilla_GearsDeserialize(Chunk_t *chunk, Gears_BufferReader *br) {
    TurboGorilla_Deserialize(chunk,
                             br,
                             (ReadUnsignedFunc)RedisGears_BRReadLong,
                             (ReadStringBufferFunc)ownedBufferFromGears);
}
