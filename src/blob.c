/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 * Copyright 2020 IoT.BzH
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "blob.h"

#include "redismodule.h"

#include <string.h>
#include "rmutil/alloc.h"

static int nbBlobs = 0;
static int nbData = 0;

TSBlob *NewBlob(const char *data, size_t len) {
    TSBlob *blob = RedisModule_Calloc(1, sizeof(TSBlob));
    memset(blob, 0, sizeof(TSBlob));
    nbBlobs++;

    if (data == NULL || len == 0)
        return blob;

    blob->len = len;
    blob->data = RedisModule_Alloc(blob->len);
    nbData++;
    memcpy(blob->data, data, blob->len);

    return blob;
}

TSBlob *BlobDup(const TSBlob *src) {
    TSBlob *blob = RedisModule_Alloc(sizeof(TSBlob));
    memset(blob, 0, sizeof(TSBlob));
    nbBlobs++;
    BlobCopy(blob, src);

    return blob;
}

void BlobCopy(TSBlob *dst, const TSBlob *src) {
    dst->len = src->len;

    if (dst->data) {
        RedisModule_Free(dst->data);
        nbData--;
    }

    dst->data = RedisModule_Alloc(src->len);

    nbData++;
    memcpy(dst->data, src->data, src->len);
}

void FreeBlob(TSBlob *blob) {
    if (blob->data) {
        nbData--;
        RedisModule_Free(blob->data);
    }

    RedisModule_Free(blob);
    nbBlobs--;
}

void RedisModule_SaveBlob(RedisModuleIO *io, const TSBlob *blob) {
    if (!blob) {
        RedisModule_SaveStringBuffer(io, EMPTY_BLOB, EMPTY_BLOB_SIZE);
        return;
    }
    RedisModule_SaveStringBuffer(io, blob->data, blob->len);
}

TSBlob *RedisModule_LoadBlob(RedisModuleIO *io) {
    size_t len;
    char *data = RedisModule_LoadStringBuffer(io, &len);
    return NewBlob(data, len);
}
