#include "generic_chunk.h"

#include "chunk.h"
#include "compressed_chunk.h"

#include "rmutil/alloc.h"

static ChunkFuncs regChunk = {
    .NewChunk = Uncompressed_NewChunk,
    .FreeChunk = Uncompressed_FreeChunk,
    .SplitChunk = Uncompressed_SplitChunk,

    .AddSample = Uncompressed_AddSample,
    .UpsertSample = Uncompressed_UpsertSample,

    .NewChunkIterator = Uncompressed_NewChunkIterator,
    .FreeChunkIterator = Uncompressed_FreeChunkIterator,
    .ChunkIteratorGetNext = Uncompressed_ChunkIteratorGetNext,
    .ChunkIteratorGetPrev = Uncompressed_ChunkIteratorGetPrev,

    .GetChunkSize = Uncompressed_GetChunkSize,
    .GetLastTimestamp = Uncompressed_GetLastTimestamp,
    .GetFirstTimestamp = Uncompressed_GetFirstTimestamp,

    .SaveToRDB = Uncompressed_SaveToRDB,
    .LoadFromRDB = Uncompressed_LoadFromRDB,
};

static ChunkFuncs comprChunk = {
    .NewChunk = Compressed_NewChunk,
    .FreeChunk = Compressed_FreeChunk,
    .SplitChunk = Compressed_SplitChunk,

    .AddSample = Compressed_AddSample,
    .UpsertSample = Compressed_UpsertSample,

    .NewChunkIterator = Compressed_NewChunkIterator,
    .FreeChunkIterator = Compressed_FreeChunkIterator,
    .ChunkIteratorGetNext = Compressed_ChunkIteratorGetNext,
    /*** Reverse iteration is on temporary decompressed chunk ***/
    .ChunkIteratorGetPrev = Uncompressed_ChunkIteratorGetPrev,

    .GetChunkSize = Compressed_GetChunkSize,
    .GetLastTimestamp = Compressed_GetLastTimestamp,
    .GetFirstTimestamp = Compressed_GetFirstTimestamp,

    .SaveToRDB = Compressed_SaveToRDB,
    .LoadFromRDB = Compressed_LoadFromRDB,
};

ChunkFuncs *GetChunkClass(CHUNK_TYPES_T chunkType) {
    switch (chunkType) {
        case CHUNK_REGULAR:
            return &regChunk;
        case CHUNK_COMPRESSED:
            return &comprChunk;
    }
    return NULL;
}
