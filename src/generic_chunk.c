#include "generic_chunk.h"
#include "chunk.h"
#include "compressed_chunk.h"


static ChunkFuncs regChunk = {
    .NewChunk = NewChunk,
    .FreeChunk = FreeChunk,

    .AddSample = ChunkAddSample,

    .NewChunkIterator = NewChunkIterator,
    .ChunkIteratorGetNext = ChunkIteratorGetNext,

    .GetChunkSize = GetChunkSize,
    .GetNumOfSample = ChunkNumOfSample,
    .GetLastTimestamp = ChunkGetLastTimestamp,
    .GetFirstTimestamp = ChunkGetFirstTimestamp
};

static ChunkFuncs comprChunk = {
    .NewChunk = CChunk_NewChunk,
    .FreeChunk = CChunk_FreeChunk,

    .AddSample = CChunk_AddSample,

    .NewChunkIterator = CChunk_NewChunkIterator,
    .ChunkIteratorGetNext = CChunk_ChunkIteratorGetNext,

    .GetChunkSize = CChunk_GetChunkSize,
    .GetNumOfSample = CChunk_ChunkNumOfSample,
    .GetLastTimestamp = CChunk_GetLastTimestamp,
    .GetFirstTimestamp = CChunk_GetFirstTimestamp
};

ChunkFuncs *GetChunkClass(int chunkType) {
  switch (chunkType) {
  case CHUNK_REGULAR:     return &regChunk;
  case CHUNK_COMPRESSED:  return &comprChunk;
  default:                return NULL;
  }
} 