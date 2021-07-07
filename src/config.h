/*
 * Copyright 2018-2020 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "parse_policies.h"
#include "redismodule.h"

#include <stdbool.h>

typedef struct
{
    SimpleCompactionRule *compactionRules;
    uint64_t compactionRulesCount;
    long long retentionPolicy;
    long long chunkSizeBytes;
    short options;
    int hasGlobalConfig;
    DuplicatePolicy duplicatePolicy;
} TSConfig;

extern TSConfig TSGlobalConfig;

int ReadConfig(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
const char *ChunkTypeToString(int options);
typedef struct RTS_RedisVersion
{
    int redisMajorVersion;
    int redisMinorVersion;
    int redisPatchVersion;
} RTS_RedisVersion;

extern RTS_RedisVersion RTS_currVersion;
extern RTS_RedisVersion RTS_minSupportedVersion;

extern int RTS_RlecMajorVersion;
extern int RTS_RlecMinorVersion;
extern int RTS_RlecPatchVersion;
extern int RTS_RlecBuild;

static inline int RTS_IsEnterprise() {
    return RTS_RlecMajorVersion != -1;
}

int RTS_CheckSupportedVestion();
void RTS_GetRedisVersion();
#endif
