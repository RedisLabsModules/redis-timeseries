/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 * Copyright 2020 IoT.BzH
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef BLOB_H
#define BLOB_H

#include "redismodule.h"

typedef struct
{
    size_t len;
    char *data;
} TSBlob;

TSBlob *NewBlob(const char *data, size_t len);
TSBlob *BlobDup(const TSBlob *src);

void BlobCopy(TSBlob *dst, const TSBlob *src);

void FreeBlob(TSBlob *blob);

void RedisModule_SaveBlob(RedisModuleIO *io, const TSBlob *blob);
TSBlob *RedisModule_LoadBlob(RedisModuleIO *io);

#define EMPTY_BLOB ""
#define EMPTY_BLOB_SIZE 1

#endif /* BLOB_H */
