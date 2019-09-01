/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#ifndef PARSE_POLICIES_H
#define PARSE_POLICIES_H

#include <sys/types.h>

typedef struct SimpleCompactionRule {
    uint64_t timeBucket;
    uint64_t retentionSizeMillisec;
    int aggType;
} SimpleCompactionRule;

int ParseCompactionPolicy(const char * policy_string, SimpleCompactionRule **parsed_rules, size_t *count_rules);
#endif
