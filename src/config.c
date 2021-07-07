/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "config.h"

#include "common.h"
#include "consts.h"
#include "redismodule.h"

#include <assert.h>
#include <string.h>
#include "rmutil/strings.h"
#include "rmutil/util.h"

TSConfig TSGlobalConfig;

int ParseDuplicatePolicy(RedisModuleCtx *ctx,
                         RedisModuleString **argv,
                         int argc,
                         const char *arg_prefix,
                         DuplicatePolicy *policy);

const char *ChunkTypeToString(int options) {
    if (options & SERIES_OPT_UNCOMPRESSED) {
        return UNCOMPRESSED_ARG_STR;
    }
    if (options & SERIES_OPT_COMPRESSED_TURBOGORILLA) {
        return COMPRESSED_TURBO_GORILLA_ARG_STR;
    }
    if (options & SERIES_OPT_COMPRESSED_GORILLA) {
        return COMPRESSED_GORILLA_ARG_STR_OLD;
    }
    return "invalid";
}

int ReadConfig(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    TSGlobalConfig.hasGlobalConfig = FALSE;
    TSGlobalConfig.options = 0;
    // default chuck type
    TSGlobalConfig.options |= SERIES_OPT_DEFAULT_COMPRESSION;

    if (argc > 1 && RMUtil_ArgIndex("COMPACTION_POLICY", argv, argc) >= 0) {
        RedisModuleString *policy;
        const char *policy_cstr;
        size_t len;

        if (RMUtil_ParseArgsAfter("COMPACTION_POLICY", argv, argc, "s", &policy) !=
            REDISMODULE_OK) {
            RedisModule_Log(ctx, "warning", "Unable to parse argument after COMPACTION_POLICY");
            return TSDB_ERROR;
        }
        policy_cstr = RedisModule_StringPtrLen(policy, &len);
        if (ParseCompactionPolicy(policy_cstr,
                                  &TSGlobalConfig.compactionRules,
                                  &TSGlobalConfig.compactionRulesCount) != TRUE) {
            RedisModule_Log(ctx, "warning", "Unable to parse argument after COMPACTION_POLICY");
            return TSDB_ERROR;
        }

        RedisModule_Log(ctx, "notice", "loaded default compaction policy: %s", policy_cstr);
        TSGlobalConfig.hasGlobalConfig = TRUE;
    }

    if (argc > 1 && RMUtil_ArgIndex("RETENTION_POLICY", argv, argc) >= 0) {
        if (RMUtil_ParseArgsAfter(
                "RETENTION_POLICY", argv, argc, "l", &TSGlobalConfig.retentionPolicy) !=
            REDISMODULE_OK) {
            RedisModule_Log(ctx, "warning", "Unable to parse argument after RETENTION_POLICY");
            return TSDB_ERROR;
        }

        RedisModule_Log(
            ctx, "notice", "loaded default retention policy: %lld", TSGlobalConfig.retentionPolicy);
        TSGlobalConfig.hasGlobalConfig = TRUE;
    } else {
        TSGlobalConfig.retentionPolicy = RETENTION_TIME_DEFAULT;
    }

    if (argc > 1 && RMUtil_ArgIndex("CHUNK_SIZE_BYTES", argv, argc) >= 0) {
        if (RMUtil_ParseArgsAfter(
                "CHUNK_SIZE_BYTES", argv, argc, "l", &TSGlobalConfig.chunkSizeBytes) !=
            REDISMODULE_OK) {
            RedisModule_Log(ctx, "warning", "Unable to parse argument after CHUNK_SIZE_BYTES");
            return TSDB_ERROR;
        }
    } else {
        TSGlobalConfig.chunkSizeBytes = Chunk_SIZE_BYTES_SECS;
    }
    RedisModule_Log(ctx,
                    "notice",
                    "loaded default CHUNK_SIZE_BYTES policy: %lld",
                    TSGlobalConfig.chunkSizeBytes);

    TSGlobalConfig.duplicatePolicy = DEFAULT_DUPLICATE_POLICY;
    if (ParseDuplicatePolicy(
            ctx, argv, argc, DUPLICATE_POLICY_ARG, &TSGlobalConfig.duplicatePolicy) != TSDB_OK) {
        RedisModule_Log(ctx, "warning", "Unable to parse argument after DUPLICATE_POLICY");
        return TSDB_ERROR;
    }
    RedisModule_Log(ctx,
                    "notice",
                    "loaded server DUPLICATE_POLICY: %s",
                    DuplicatePolicyToString(TSGlobalConfig.duplicatePolicy));

    if (argc > 1 && RMUtil_ArgIndex("CHUNK_TYPE", argv, argc) >= 0) {
        RedisModuleString *chunk_type;
        size_t len;
        const char *chunk_type_cstr;
        if (RMUtil_ParseArgsAfter("CHUNK_TYPE", argv, argc, "s", &chunk_type) != REDISMODULE_OK) {
            RedisModule_Log(ctx, "warning", "Unable to parse argument after CHUNK_TYPE");
            return TSDB_ERROR;
        }
        RMUtil_StringToLower(chunk_type);
        chunk_type_cstr = RedisModule_StringPtrLen(chunk_type, &len);

        if (strncmp(chunk_type_cstr, COMPRESSED_GORILLA_ARG_STR, len) == 0 ||
            strncmp(chunk_type_cstr, COMPRESSED_GORILLA_ARG_STR_OLD, len) == 0) {
            TSGlobalConfig.options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            TSGlobalConfig.options |= SERIES_OPT_COMPRESSED_GORILLA;
        } else if (strncmp(chunk_type_cstr, COMPRESSED_TURBO_GORILLA_ARG_STR, len) == 0) {
            TSGlobalConfig.options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            TSGlobalConfig.options |= SERIES_OPT_COMPRESSED_TURBOGORILLA;
        } else if (strncmp(chunk_type_cstr, UNCOMPRESSED_ARG_STR, len) == 0) {
            TSGlobalConfig.options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            TSGlobalConfig.options |= SERIES_OPT_UNCOMPRESSED;
        } else {
            printf("ERR\n");
            RedisModule_Log(ctx, "warning", "unknown chunk type: %s\n", chunk_type_cstr);
            return TSDB_ERROR;
        }
    }
    RedisModule_Log(
        ctx, "notice", "Setting default chunk type: %s", ChunkTypeToString(TSGlobalConfig.options));
    return TSDB_OK;
}

RTS_RedisVersion RTS_currVersion;

RTS_RedisVersion RTS_minSupportedVersion = {
    .redisMajorVersion = 5,
    .redisMinorVersion = 0,
    .redisPatchVersion = 0,
};

int RTS_RlecMajorVersion;
int RTS_RlecMinorVersion;
int RTS_RlecPatchVersion;
int RTS_RlecBuild;

int RTS_CheckSupportedVestion() {
    if (RTS_currVersion.redisMajorVersion < RTS_minSupportedVersion.redisMajorVersion) {
        return REDISMODULE_ERR;
    }

    if (RTS_currVersion.redisMajorVersion == RTS_minSupportedVersion.redisMajorVersion) {
        if (RTS_currVersion.redisMinorVersion < RTS_minSupportedVersion.redisMinorVersion) {
            return REDISMODULE_ERR;
        }

        if (RTS_currVersion.redisMinorVersion == RTS_minSupportedVersion.redisMinorVersion) {
            if (RTS_currVersion.redisPatchVersion < RTS_minSupportedVersion.redisPatchVersion) {
                return REDISMODULE_ERR;
            }
        }
    }

    return REDISMODULE_OK;
}

void RTS_GetRedisVersion() {
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, "info", "c", "server");
    assert(RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_STRING);
    size_t len;
    const char *replyStr = RedisModule_CallReplyStringPtr(reply, &len);

    int n = sscanf(replyStr,
                   "# Server\nredis_version:%d.%d.%d",
                   &RTS_currVersion.redisMajorVersion,
                   &RTS_currVersion.redisMinorVersion,
                   &RTS_currVersion.redisPatchVersion);
    if (n != 3) {
        RedisModule_Log(NULL, "warning", "Could not extract redis version");
    }

    RTS_RlecMajorVersion = -1;
    RTS_RlecMinorVersion = -1;
    RTS_RlecPatchVersion = -1;
    RTS_RlecBuild = -1;
    const char *enterpriseStr = strstr(replyStr, "rlec_version:");
    if (enterpriseStr) {
        n = sscanf(enterpriseStr,
                   "rlec_version:%d.%d.%d-%d",
                   &RTS_RlecMajorVersion,
                   &RTS_RlecMinorVersion,
                   &RTS_RlecPatchVersion,
                   &RTS_RlecBuild);
        if (n != 4) {
            RedisModule_Log(NULL, "warning", "Could not extract enterprise version");
        }
    }

    RedisModule_FreeCallReply(reply);
    RedisModule_FreeThreadSafeContext(ctx);
}
