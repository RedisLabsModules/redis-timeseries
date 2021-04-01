
#include "gears_commands.h"

#include "consts.h"
#include "gears_integration.h"
#include "query_language.h"
#include "redisgears.h"
#include "reply.h"
#include "resultset.h"

#include "rmutil/alloc.h"

static void mget_done(ExecutionPlan *gearsCtx, void *privateData) {
    RedisModuleBlockedClient *bc = privateData;
    RedisModuleCtx *rctx = RedisModule_GetThreadSafeContext(bc);

    long long len = RedisGears_GetRecordsLen(gearsCtx);
    RedisModule_ReplyWithArray(rctx, len);
    for (int i = 0; i < len; i++) {
        Record *r = RedisGears_GetRecord(gearsCtx, i);
        RedisGears_RecordSendReply(r, rctx);
    }

    RedisModule_UnblockClient(bc, NULL);
    RedisGears_DropExecution(gearsCtx);
    RedisModule_FreeThreadSafeContext(rctx);
}

static void mrange_done(ExecutionPlan *gearsCtx, void *privateData) {
    MRangeData *data = privateData;
    RedisModuleBlockedClient *bc = data->bc;
    RedisModuleCtx *rctx = RedisModule_GetThreadSafeContext(bc);

    long long len = RedisGears_GetRecordsLen(gearsCtx);

    TS_ResultSet *resultset = NULL;

    if (data->args.groupByLabel) {
        resultset = ResultSet_Create();
        ResultSet_GroupbyLabel(resultset, data->args.groupByLabel);
    } else {
        RedisModule_ReplyWithArray(rctx, len);
    }

    Series **tempSeries = calloc(len, sizeof(Series *));
    for (int i = 0; i < len; i++) {
        Record *raw_record = RedisGears_GetRecord(gearsCtx, i);
        if (raw_record->type != GetSeriesRecordType()) {
            continue;
        }
        Series *s = SeriesRecord_IntoSeries((SeriesRecord *)raw_record);
        tempSeries[i] = s;

        if (data->args.groupByLabel) {
            ResultSet_AddSerie(resultset, s, RedisModule_StringPtrLen(s->keyName, NULL));
        } else {
            ReplySeriesArrayPos(rctx,
                                s,
                                data->args.withLabels,
                                data->args.startTimestamp,
                                data->args.endTimestamp,
                                data->args.aggregationArgs.aggregationClass,
                                data->args.aggregationArgs.timeDelta,
                                data->args.count,
                                data->args.reverse);
        }
    }

    if (data->args.groupByLabel) {
        // Apply the reducer
        ResultSet_ApplyReducer(resultset,
                               data->args.startTimestamp,
                               data->args.endTimestamp,
                               data->args.aggregationArgs.aggregationClass,
                               data->args.aggregationArgs.timeDelta,
                               -1,
                               false,
                               data->args.gropuByReducerOp);

        // Do not apply the aggregation on the resultset, do apply max results on the final result
        replyResultSet(rctx,
                       resultset,
                       data->args.withLabels,
                       data->args.startTimestamp,
                       data->args.endTimestamp,
                       NULL,
                       0,
                       data->args.count,
                       data->args.reverse);

        ResultSet_Free(resultset);
    }
    RedisModule_UnblockClient(bc, NULL);
    for (int i = 0; i < len; i++) {
        FreeSeries(tempSeries[i]);
    }
    free(tempSeries);
    MRangeArgs_Free(&data->args);
    free(data);
    RedisGears_DropExecution(gearsCtx);
    RedisModule_FreeThreadSafeContext(rctx);
}

int TSDB_mget_RG(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    int filter_location = RMUtil_ArgIndex("FILTER", argv, argc);
    if (filter_location == -1) {
        return RedisModule_WrongArity(ctx);
    }
    size_t query_count = argc - 1 - filter_location;
    const int withlabels_location = RMUtil_ArgIndex("WITHLABELS", argv, argc);
    int response;
    QueryPredicateList *queries =
        parseLabelListFromArgs(ctx, argv, filter_location + 1, query_count, &response);
    if (response == TSDB_ERROR) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
    }

    if (CountPredicateType(queries, EQ) + CountPredicateType(queries, LIST_MATCH) == 0) {
        QueryPredicateList_Free(queries);
        return RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
    }

    char *err = NULL;
    FlatExecutionPlan *rg_ctx = RedisGears_CreateCtx("ShardIDReader", &err);
    if (err) {
        RedisModule_ReplyWithError(ctx, err);
    }
    QueryPredicates_Arg *queryArg = malloc(sizeof(QueryPredicates_Arg));
    queryArg->count = queries->count;
    queryArg->startTimestamp = 0;
    queryArg->endTimestamp = 0;
    // moving ownership of queries to QueryPredicates_Arg
    queryArg->predicates = queries;
    queryArg->withLabels = (withlabels_location > 0);
    RedisGears_FlatMap(rg_ctx, "ShardMgetMapper", queryArg);

    RGM_Collect(rg_ctx);

    ExecutionPlan *ep = RGM_Run(rg_ctx, ExecutionModeAsync, NULL, NULL, NULL, &err);
    if (!ep) {
        RedisGears_FreeFlatExecution(rg_ctx);
        RedisModule_ReplyWithError(ctx, err);
        return REDISMODULE_OK;
    }

    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    RedisGears_AddOnDoneCallback(ep, mget_done, bc);
    RedisGears_FreeFlatExecution(rg_ctx);
    return REDISMODULE_OK;
}

int TSDB_mrange_RG(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool reverse) {
    MRangeArgs args;
    if (parseMRangeCommand(ctx, argv, argc, &args) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    args.reverse = reverse;

    char *err = NULL;
    FlatExecutionPlan *rg_ctx = RedisGears_CreateCtx("ShardIDReader", &err);
    if (err) {
        RedisModule_ReplyWithError(ctx, err);
    }
    QueryPredicates_Arg *queryArg = malloc(sizeof(QueryPredicates_Arg));
    queryArg->count = args.queryPredicates->count;
    queryArg->startTimestamp = args.startTimestamp;
    queryArg->endTimestamp = args.endTimestamp;
    args.queryPredicates->ref++;
    queryArg->predicates = args.queryPredicates;
    queryArg->withLabels = args.withLabels;
    RedisGears_FlatMap(rg_ctx, "ShardSeriesMapper", queryArg);
    RGM_Collect(rg_ctx);

    ExecutionPlan *ep = RGM_Run(rg_ctx, ExecutionModeAsync, NULL, NULL, NULL, &err);
    if (!ep) {
        RedisGears_FreeFlatExecution(rg_ctx);
        RedisModule_ReplyWithError(ctx, err);
        return REDISMODULE_OK;
    }

    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    MRangeData *data = malloc(sizeof(struct MRangeData));
    data->bc = bc;
    data->args = args;
    RedisGears_AddOnDoneCallback(ep, mrange_done, data);
    RedisGears_FreeFlatExecution(rg_ctx);
    return REDISMODULE_OK;
}

int TSDB_queryindex_RG(RedisModuleCtx *ctx, QueryPredicateList *queries) {
    char *err = NULL;
    FlatExecutionPlan *rg_ctx = RedisGears_CreateCtx("ShardIDReader", &err);
    if (err) {
        RedisModule_ReplyWithError(ctx, err);
    }
    QueryPredicates_Arg *queryArg = malloc(sizeof(QueryPredicates_Arg));
    queryArg->count = queries->count;
    queryArg->startTimestamp = 0;
    queryArg->endTimestamp = 0;
    queries->ref++;
    queryArg->predicates = queries;
    queryArg->withLabels = false;
    RedisGears_FlatMap(rg_ctx, "ShardQueryindexMapper", queryArg);

    RGM_Collect(rg_ctx);

    ExecutionPlan *ep = RGM_Run(rg_ctx, ExecutionModeAsync, NULL, NULL, NULL, &err);
    if (!ep) {
        RedisGears_FreeFlatExecution(rg_ctx);
        RedisModule_ReplyWithError(ctx, err);
        return REDISMODULE_OK;
    }

    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    RedisGears_AddOnDoneCallback(ep, mget_done, bc);
    RedisGears_FreeFlatExecution(rg_ctx);
    return REDISMODULE_OK;
}
