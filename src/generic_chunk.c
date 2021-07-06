#include "generic_chunk.h"

#include "chunk.h"
#include "compressed_chunk.h"

#include <ctype.h>
#include "rmutil/alloc.h"

static ChunkFuncs regChunk = {
    .NewChunk = Uncompressed_NewChunk,
    .FreeChunk = Uncompressed_FreeChunk,
    .SplitChunk = Uncompressed_SplitChunk,

    .AddSample = Uncompressed_AddSample,
    .UpsertSample = Uncompressed_UpsertSample,
    .DelRange = Uncompressed_DelRange,

    .NewChunkIterator = Uncompressed_NewChunkIterator,

    .GetChunkSize = Uncompressed_GetChunkSize,
    .GetNumOfSample = Uncompressed_NumOfSample,
    .GetLastTimestamp = Uncompressed_GetLastTimestamp,
    .GetFirstTimestamp = Uncompressed_GetFirstTimestamp,

    .SaveToRDB = Uncompressed_SaveToRDB,
    .LoadFromRDB = Uncompressed_LoadFromRDB,
    .GearsSerialize = Uncompressed_GearsSerialize,
    .GearsDeserialize = Uncompressed_GearsDeserialize,
};

ChunkIterFuncs uncompressedChunkIteratorClass = {
    .Free = Uncompressed_FreeChunkIterator,
    .GetNext = Uncompressed_ChunkIteratorGetNext,
    .GetPrev = Uncompressed_ChunkIteratorGetPrev,
};

static ChunkFuncs comprChunk = {
    .NewChunk = Compressed_NewChunk,
    .FreeChunk = Compressed_FreeChunk,
    .CloneChunk = Compressed_CloneChunk,
    .SplitChunk = Compressed_SplitChunk,

    .AddSample = Compressed_AddSample,
    .UpsertSample = Compressed_UpsertSample,
    .DelRange = Compressed_DelRange,

    .NewChunkIterator = Compressed_NewChunkIterator,

    .GetChunkSize = Compressed_GetChunkSize,
    .GetNumOfSample = Compressed_ChunkNumOfSample,
    .GetLastTimestamp = Compressed_GetLastTimestamp,
    .GetFirstTimestamp = Compressed_GetFirstTimestamp,

    .SaveToRDB = Compressed_SaveToRDB,
    .LoadFromRDB = Compressed_LoadFromRDB,
    .GearsSerialize = Compressed_GearsSerialize,
    .GearsDeserialize = Compressed_GearsDeserialize,
};

static ChunkIterFuncs compressedChunkIteratorClass = {
    .Free = Compressed_FreeChunkIterator,
    .GetNext = Compressed_ChunkIteratorGetNext,
    /*** Reverse iteration is on temporary decompressed chunk ***/
    .GetPrev = NULL,
};

// This function will decide according to the policy how to handle duplicate sample, the `newSample`
// will contain the data that will be kept in the database.
ChunkResult handleDuplicateSample(DuplicatePolicy policy, Sample oldSample, Sample *newSample) {
    switch (policy) {
        case DP_BLOCK:
            return CR_ERR;
        case DP_FIRST:
            *newSample = oldSample;
            return CR_OK;
        case DP_LAST:
            return CR_OK;
        case DP_MIN:
            if (VALUE_DOUBLE(&oldSample.value) < VALUE_DOUBLE(&newSample->value))
                VALUE_DOUBLE(&newSample->value) = VALUE_DOUBLE(&oldSample.value);
            return CR_OK;
        case DP_MAX:
            if (VALUE_DOUBLE(&oldSample.value) > VALUE_DOUBLE(&newSample->value))
                VALUE_DOUBLE(&newSample->value) = VALUE_DOUBLE(&oldSample.value);
            return CR_OK;
        case DP_SUM:
            VALUE_DOUBLE(&newSample->value) += VALUE_DOUBLE(&oldSample.value);
            return CR_OK;
        default:
            return CR_ERR;
    }
}

ChunkFuncs *GetChunkClass(CHUNK_TYPES_T chunkType) {
    switch (chunkType) {
        case CHUNK_REGULAR:
            return &regChunk;
        case CHUNK_COMPRESSED:
            return &comprChunk;
    }
    return NULL;
}

ChunkIterFuncs *GetChunkIteratorClass(CHUNK_TYPES_T chunkType) {
    switch (chunkType) {
        case CHUNK_REGULAR:
            return &uncompressedChunkIteratorClass;
        case CHUNK_COMPRESSED:
            return &compressedChunkIteratorClass;
    }
    return NULL;
}

const char *DuplicatePolicyToString(DuplicatePolicy policy) {
    switch (policy) {
        case DP_NONE:
            return "none";
        case DP_BLOCK:
            return "block";
        case DP_LAST:
            return "last";
        case DP_FIRST:
            return "first";
        case DP_MAX:
            return "max";
        case DP_MIN:
            return "min";
        case DP_SUM:
            return "sum";
        default:
            return "invalid";
    }
}

int RMStringLenDuplicationPolicyToEnum(RedisModuleString *aggTypeStr) {
    size_t str_len;
    const char *aggTypeCStr = RedisModule_StringPtrLen(aggTypeStr, &str_len);
    return DuplicatePolicyFromString(aggTypeCStr, str_len);
}

DuplicatePolicy DuplicatePolicyFromString(const char *input, size_t len) {
    char input_lower[len];
    for (int i = 0; i < len; i++) {
        input_lower[i] = tolower(input[i]);
    }
    if (len == 3) {
        if (strncmp(input_lower, "min", len) == 0) {
            return DP_MIN;
        } else if (strncmp(input_lower, "max", len) == 0) {
            return DP_MAX;
        } else if (strncmp(input_lower, "sum", len) == 0) {
            return DP_SUM;
        }
    } else if (len == 4) {
        if (strncmp(input_lower, "last", len) == 0) {
            return DP_LAST;
        }
    } else if (len == 5) {
        if (strncmp(input_lower, "block", len) == 0) {
            return DP_BLOCK;
        } else if (strncmp(input_lower, "first", len) == 0) {
            return DP_FIRST;
        }
    }
    return DP_INVALID;
}

void updateSampleValue(bool isblob, SampleValue *dest, const SampleValue *src) {
    if (!isblob) {
        VALUE_DOUBLE(dest) = VALUE_DOUBLE(src);
        return;
    }

    TSBlob *dstBlob = VALUE_BLOB(dest);
    TSBlob *srcBlob = VALUE_BLOB(src);

    BlobCopy(dstBlob, srcBlob);
}
