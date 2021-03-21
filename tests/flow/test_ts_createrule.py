import math
import random
import statistics

import pytest
import redis
from RLTest import Env
from test_helper_classes import _get_series_value, calc_rule, ALLOWED_ERROR, _insert_data, \
    _get_ts_info, _insert_agg_data

key_name = 'tester{abc}'
agg_key_name = '{}_agg_max_10'.format(key_name)

def test_compaction_rules(self):
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name, 'CHUNK_SIZE', '360')
        assert r.execute_command('TS.CREATE', agg_key_name)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', -10)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', 0)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', 10)

        start_ts = 1488823384
        samples_count = 1500
        _insert_data(r, key_name, start_ts, samples_count, 5)
        last_ts = start_ts + samples_count + 10
        r.execute_command('TS.ADD', key_name, last_ts, 5)

        actual_result = r.execute_command('TS.RANGE', agg_key_name, start_ts, start_ts + samples_count)

        assert len(actual_result) == samples_count / 10

        info = _get_ts_info(r, key_name)
        assert info.rules == [[agg_key_name.encode('ascii'), 10, b'AVG']]


def test_create_compaction_rule_with_wrong_aggregation():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATE', agg_key_name)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAXX', 10)

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MA', 10)


def test_create_compaction_rule_without_dest_series():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAX', 10)


def test_create_compaction_rule_twice():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAX', 10)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAX', 10)


def test_create_compaction_rule_override_dest():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATE', 'tester2')
        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAX', 10)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', 'tester2', agg_key_name, 'AGGREGATION', 'MAX', 10)


def test_create_compaction_rule_from_target():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATE', 'tester2')
        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'MAX', 10)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', agg_key_name, 'tester2', 'AGGREGATION', 'MAX', 10)


def test_create_compaction_rule_own():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE', key_name, key_name, 'AGGREGATION', 'MAX', 10)


def test_create_compaction_rule_and_del_dest_series():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'AVG', 10)
        assert r.delete(agg_key_name)

        start_ts = 1488823384
        samples_count = 1500
        _insert_data(r, key_name, start_ts, samples_count, 5)


def test_std_var_func():
    with Env().getClusterConnectionIfNeeded() as r:
        raw_key = 'raw{abc}'
        std_key = 'std_key{abc}'
        var_key = 'var_key{abc}'

        random_numbers = 100
        random.seed(0)
        items = random.sample(range(random_numbers), random_numbers)

        stdev = statistics.stdev(items)
        var = statistics.variance(items)
        assert r.execute_command('TS.CREATE', raw_key)
        assert r.execute_command('TS.CREATE', std_key)
        assert r.execute_command('TS.CREATE', var_key)
        assert r.execute_command('TS.CREATERULE', raw_key, std_key, "AGGREGATION", 'std.s', random_numbers)
        assert r.execute_command('TS.CREATERULE', raw_key, var_key, "AGGREGATION", 'var.s', random_numbers)

        for i in range(random_numbers):
            r.execute_command('TS.ADD', raw_key, i, items[i])
        r.execute_command('TS.ADD', raw_key, random_numbers, 0)  # close time bucket

        assert abs(stdev - float(r.execute_command('TS.GET', std_key)[1])) < ALLOWED_ERROR
        assert abs(var - float(r.execute_command('TS.GET', var_key)[1])) < ALLOWED_ERROR


def test_delete_key():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', key_name, 'CHUNK_SIZE', '360')
        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', 10)
        assert r.delete(agg_key_name)
        assert _get_ts_info(r, key_name).rules == []

        assert r.execute_command('TS.CREATE', agg_key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', 11)
        assert r.delete(key_name)
        assert _get_ts_info(r, agg_key_name).sourceKey == None

        assert r.execute_command('TS.CREATE', key_name)
        assert r.execute_command('TS.CREATERULE', key_name, agg_key_name, 'AGGREGATION', 'avg', 12)
        assert _get_ts_info(r, key_name).rules == [[agg_key_name.encode('ascii'), 12, b'AVG']]


def test_downsampling_current():
    with Env().getClusterConnectionIfNeeded() as r:
        key = 'src{a}'
        agg_key = 'dest{a}'
        type_list = ['', 'uncompressed']
        agg_list = ['avg', 'sum', 'min', 'max', 'count', 'range', 'first', 'last', 'std.p', 'std.s', 'var.p',
                    'var.s']  # more
        for chunk_type in type_list:
            for agg_type in agg_list:
                assert r.execute_command('TS.CREATE', key, chunk_type, "DUPLICATE_POLICY", "LAST")
                assert r.execute_command('TS.CREATE', agg_key, chunk_type)
                assert r.execute_command('TS.CREATERULE', key, agg_key, "AGGREGATION", agg_type, 10)

                # present update
                assert r.execute_command('TS.ADD', key, 3, 3) == 3
                assert r.execute_command('TS.ADD', key, 5, 5) == 5
                assert r.execute_command('TS.ADD', key, 7, 7) == 7
                assert r.execute_command('TS.ADD', key, 5, 2) == 5
                assert r.execute_command('TS.ADD', key, 10, 10) == 10

                expected_result = r.execute_command('TS.RANGE', key, 0, -1, 'aggregation', agg_type, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 0, -1)
                assert expected_result[0] == actual_result[0]

                # present add
                assert r.execute_command('TS.ADD', key, 11, 11) == 11
                assert r.execute_command('TS.ADD', key, 15, 15) == 15
                assert r.execute_command('TS.ADD', key, 14, 14) == 14
                assert r.execute_command('TS.ADD', key, 20, 20) == 20

                expected_result = r.execute_command('TS.RANGE', key, 0, -1, 'aggregation', agg_type, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 0, -1)
                assert expected_result[0:1] == actual_result[0:1]

                # present + past add
                assert r.execute_command('TS.ADD', key, 23, 23) == 23
                assert r.execute_command('TS.ADD', key, 15, 22) == 15
                assert r.execute_command('TS.ADD', key, 27, 27) == 27
                assert r.execute_command('TS.ADD', key, 23, 25) == 23
                assert r.execute_command('TS.ADD', key, 30, 30) == 30

                expected_result = r.execute_command('TS.RANGE', key, 0, -1, 'aggregation', agg_type, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 0, -1)
                assert expected_result[0:3] == actual_result[0:3]
                assert 3 == _get_ts_info(r, agg_key).total_samples
                assert 11 == _get_ts_info(r, key).total_samples

                r.execute_command('DEL', key)
                r.execute_command('DEL', agg_key)


def test_downsampling_extensive():
    with Env().getClusterConnectionIfNeeded() as r:
        key = 'tester{abc}'
        fromTS = 10
        toTS = 10000
        type_list = ['', 'uncompressed']
        for chunk_type in type_list:
            agg_list = ['avg', 'sum', 'min', 'max', 'count', 'range', 'first', 'last', 'std.p', 'std.s', 'var.p',
                        'var.s']  # more
            for agg in agg_list:
                agg_key = _insert_agg_data(r, key, agg, chunk_type, fromTS, toTS,
                                           key_create_args=['DUPLICATE_POLICY', 'LAST'])

                # sanity + check result have changed
                expected_result1 = r.execute_command('TS.RANGE', key, fromTS, toTS, 'aggregation', agg, 10)
                actual_result1 = r.execute_command('TS.RANGE', agg_key, fromTS, toTS)
                assert expected_result1 == actual_result1
                assert len(expected_result1) == 999

                for i in range(fromTS + 5, toTS - 4, 10):
                    assert r.execute_command('TS.ADD', key, i, 42)

                expected_result2 = r.execute_command('TS.RANGE', key, fromTS, toTS, 'aggregation', agg, 10)
                actual_result2 = r.execute_command('TS.RANGE', agg_key, fromTS, toTS)
                assert expected_result2 == actual_result2

                # remove aggs with identical results
                compare_list = ['avg', 'sum', 'min', 'range', 'std.p', 'std.s', 'var.p', 'var.s']
                if agg in compare_list:
                    assert expected_result1 != expected_result2
                    assert actual_result1 != actual_result2

                r.execute_command('DEL', key)
                r.execute_command('DEL', agg_key)


def test_downsampling_rules(self):
    """
    Test downsmapling rules - avg,min,max,count,sum with 4 keys each.
    Downsample in resolution of:
    1sec (should be the same length as the original series),
    3sec (number of samples is divisible by 10),
    10s (number of samples is not divisible by 10),
    1000sec (series should be empty since there are not enough samples)
    Insert some data and check that the length, the values and the info of the downsample series are as expected.
    """
    with Env().getClusterConnectionIfNeeded() as r:
        key = 'tester{abc}'
        assert r.execute_command('TS.CREATE', key)
        rules = ['avg', 'sum', 'count', 'max', 'min']
        resolutions = [1, 3, 10, 1000]
        for rule in rules:
            for resolution in resolutions:
                agg_key = '{}_{}_{}'.format(key, rule, resolution)
                assert r.execute_command('TS.CREATE', agg_key)
                assert r.execute_command('TS.CREATERULE', key, agg_key, 'AGGREGATION', rule, resolution)

        start_ts = 0
        samples_count = 501
        end_ts = start_ts + samples_count
        values = list(range(samples_count))
        _insert_data(r, key, start_ts, samples_count, values)
        r.execute_command('TS.ADD', key, 3000, 7.77)

        for rule in rules:
            for resolution in resolutions:
                actual_result = r.execute_command('TS.RANGE', '{}_{}_{}'.format(key, rule, resolution),
                                                  start_ts, end_ts)
                assert len(actual_result) == math.ceil(samples_count / float(resolution))
                expected_result = calc_rule(rule, values, resolution)
                assert _get_series_value(actual_result) == expected_result
                # last time stamp should be the beginning of the last bucket
                assert _get_ts_info(r, '{}_{}_{}'.format(key, rule, resolution)).last_time_stamp == \
                       (samples_count - 1) - (samples_count - 1) % resolution

        # test for results after empty buckets
        r.execute_command('TS.ADD', key, 6000, 0)
        for rule in rules:
            for resolution in resolutions:
                actual_result = r.execute_command('TS.RANGE', '{}_{}_{}'.format(key, rule, resolution),
                                                  3000, 6000)
                assert len(actual_result) == 1
                assert _get_series_value(actual_result) == [7.77] or \
                       _get_series_value(actual_result) == [1]


def test_backfill_downsampling(self):
    with Env().getClusterConnectionIfNeeded() as r:
        key = 'tester{a}'
        type_list = ['', 'uncompressed']
        for chunk_type in type_list:
            agg_list = ['sum', 'min', 'max', 'count', 'first', 'last']  # more
            for agg in agg_list:
                agg_key = _insert_agg_data(r, key, agg, chunk_type, key_create_args=['DUPLICATE_POLICY', 'LAST'])

                expected_result = r.execute_command('TS.RANGE', key, 10, 50, 'aggregation', agg, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
                assert expected_result == actual_result
                assert r.execute_command('TS.ADD', key, 15, 50) == 15
                expected_result = r.execute_command('TS.RANGE', key, 10, 50, 'aggregation', agg, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
                assert expected_result == actual_result

                # add in latest window
                r.execute_command('TS.ADD', key, 1055, 50) == 1055
                r.execute_command('TS.ADD', key, 1053, 55) == 1053
                r.execute_command('TS.ADD', key, 1062, 60) == 1062
                expected_result = r.execute_command('TS.RANGE', key, 10, 1060, 'aggregation', agg, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 10, 1060)
                assert expected_result == actual_result

                # update in latest window
                r.execute_command('TS.ADD', key, 1065, 65) == 1065
                r.execute_command('TS.ADD', key, 1066, 66) == 1066
                r.execute_command('TS.ADD', key, 1001, 42) == 1001
                r.execute_command('TS.ADD', key, 1075, 50) == 1075
                expected_result = r.execute_command('TS.RANGE', key, 10, 1070, 'aggregation', agg, 10)
                actual_result = r.execute_command('TS.RANGE', agg_key, 10, 1070)
                assert expected_result == actual_result

                r.execute_command('DEL', key)
                r.execute_command('DEL', agg_key)


def test_rule_timebucket_64bit(self):
    Env().skipOnCluster()
    with Env().getClusterConnectionIfNeeded() as r:
        BELOW_32BIT_LIMIT = 2147483647
        ABOVE_32BIT_LIMIT = 2147483648
        r.execute_command("ts.create", 'test_key', 'RETENTION', ABOVE_32BIT_LIMIT)
        r.execute_command("ts.create", 'below_32bit_limit')
        r.execute_command("ts.create", 'above_32bit_limit')
        r.execute_command("ts.createrule", 'test_key', 'below_32bit_limit', 'AGGREGATION', 'max', BELOW_32BIT_LIMIT)
        r.execute_command("ts.createrule", 'test_key', 'above_32bit_limit', 'AGGREGATION', 'max', ABOVE_32BIT_LIMIT)
        info = _get_ts_info(r, 'test_key')
        assert info.rules[0][1] == BELOW_32BIT_LIMIT
        assert info.rules[1][1] == ABOVE_32BIT_LIMIT
