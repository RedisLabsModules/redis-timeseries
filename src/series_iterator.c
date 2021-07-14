/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "series_iterator.h"

#include "abstract_iterator.h"
#include "filter_iterator.h"
#include "tsdb.h"

static int SeriesChunkIteratorOptions(SeriesIterator *iter) {
    int options = 0;
    if (iter->reverse) {
        options |= CHUNK_ITER_OP_REVERSE;
    }
    return options;
}

// Initiates SeriesIterator, find the correct chunk and initiate a ChunkIterator
AbstractIterator *SeriesIterator_New(Series *series,
                                     timestamp_t start_ts,
                                     timestamp_t end_ts,
                                     bool rev) {
    SeriesIterator *iter = malloc(sizeof(SeriesIterator));
    iter->base.Close = SeriesIteratorClose;
    iter->base.GetNext = SeriesIteratorGetNext;
    iter->base.input = NULL;
    iter->currentChunk = NULL;
    iter->chunkIterator = NULL;

    iter->series = series;
    iter->minTimestamp = start_ts;
    iter->maxTimestamp = end_ts;
    iter->reverse = rev;

    timestamp_t rax_key;
    ChunkFuncs *funcs = series->funcs;

    if (iter->reverse == false) {
        iter->DictGetNext = RedisModule_DictNextC;
        seriesEncodeTimestamp(&rax_key, iter->minTimestamp);
    } else {
        iter->DictGetNext = RedisModule_DictPrevC;
        seriesEncodeTimestamp(&rax_key, iter->maxTimestamp);
    }

    // get first chunk within query range
    iter->dictIter =
        RedisModule_DictIteratorStartC(series->chunks, "<=", &rax_key, sizeof(rax_key));
    if (!iter->DictGetNext(iter->dictIter, NULL, (void *)&iter->currentChunk)) {
        RedisModule_DictIteratorReseekC(iter->dictIter, "^", NULL, 0);
        iter->DictGetNext(iter->dictIter, NULL, (void *)&iter->currentChunk);
    }

    if (iter->currentChunk != NULL) {
        iter->chunkIterator = funcs->NewChunkIterator(
            iter->currentChunk, SeriesChunkIteratorOptions(iter), &iter->chunkIteratorFuncs);
    }

    return (AbstractIterator *)iter;
}

// this is an internal function that routes the next call to the appropriate chunk iterator function
static inline ChunkResult SeriesGetNext(SeriesIterator *iter, Sample *sample) {
    return iter->chunkIteratorFuncs.GetNext(iter->chunkIterator, sample);
}

// this is an internal function that routes the next call to the appropriate chunk iterator function
static inline ChunkResult SeriesGetPrevious(SeriesIterator *iter, Sample *sample) {
    return iter->chunkIteratorFuncs.GetPrev(iter->chunkIterator, sample);
}

void SeriesIteratorClose(AbstractIterator *iterator) {
    SeriesIterator *self = (SeriesIterator *)iterator;
    if (self->chunkIterator != NULL) {
        self->chunkIteratorFuncs.Free(self->chunkIterator);
    }

    RedisModule_DictIteratorStop(self->dictIter);
    free(iterator);
}

// Fills sample from chunk. If all samples were extracted from the chunk, we
// move to the next chunk.
ChunkResult SeriesIteratorGetNext(AbstractIterator *abstractIterator, Sample *currentSample) {
    SeriesIterator *iterator = (SeriesIterator *)abstractIterator;
    if (unlikely(iterator->chunkIterator == NULL)) {
        return CR_END;
    }
    ChunkResult res;
    ChunkFuncs *funcs = iterator->series->funcs;
    Chunk_t *currentChunk = iterator->currentChunk;
    const uint64_t itt_max_ts = iterator->maxTimestamp;
    const uint64_t itt_min_ts = iterator->minTimestamp;
    const int not_reverse = !iterator->reverse;
    timestamp_t timestamp = 0;
    if (likely(not_reverse)) {
        while (TRUE) {
            res = SeriesGetNext(iterator, currentSample);
            if (unlikely(res == CR_END)) { // Reached the end of the chunk
                if (!iterator->DictGetNext(iterator->dictIter, NULL, (void *)&currentChunk) ||
                    funcs->GetFirstTimestamp(currentChunk) > itt_max_ts ||
                    funcs->GetLastTimestamp(currentChunk) < itt_min_ts) {
                    return CR_END; // No more chunks or they out of range
                }
                iterator->chunkIteratorFuncs.Reset(iterator->chunkIterator, currentChunk);
                if (SeriesGetNext(iterator, currentSample) != CR_OK) {
                    return CR_END;
                }
            } else if (res == CR_ERR) {
                return CR_ERR;
            }
            // check timestamp is within range
            // forward range handling
            timestamp = currentSample->timestamp;
            if (timestamp < itt_min_ts) {
                // didn't reach the starting point of the requested range
                continue;
            }
            if (timestamp > itt_max_ts) {
                // reached the end of the requested range
                return CR_END;
            }
            return CR_OK;
        }
    } else {
        while (TRUE) {
            res = SeriesGetPrevious(iterator, currentSample);
            if (unlikely(res == CR_END)) { // Reached the end of the chunk
                if (!iterator->DictGetNext(iterator->dictIter, NULL, (void *)&currentChunk) ||
                    funcs->GetFirstTimestamp(currentChunk) > itt_max_ts ||
                    funcs->GetLastTimestamp(currentChunk) < itt_min_ts) {
                    return CR_END; // No more chunks or they out of range
                }
                iterator->chunkIteratorFuncs.Free(iterator->chunkIterator);
                iterator->chunkIterator =
                    funcs->NewChunkIterator(currentChunk,
                                            SeriesChunkIteratorOptions(iterator),
                                            &iterator->chunkIteratorFuncs);
                if (SeriesGetPrevious(iterator, currentSample) != CR_OK) {
                    return CR_END;
                }
            } else if (res == CR_ERR) {
                return CR_ERR;
            }
            // reverse range handling
            if (currentSample->timestamp > itt_max_ts) {
                // didn't reach our starting range
                continue;
            }
            if (currentSample->timestamp < itt_min_ts) {
                // didn't reach the starting point of the requested range
                return CR_END;
            }
            return CR_OK;
        }
    }
    return CR_OK;
}
