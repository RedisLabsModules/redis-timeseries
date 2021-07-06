import math
import time

import pytest
import redis
from RLTest import Env
from test_helper_classes import SAMPLE_SIZE, _get_ts_info


def test_create_params():
    with Env().getClusterConnectionIfNeeded() as r:
        # test string instead of value
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE', 'invalid', 'RETENTION', 'retention')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE', 'invalid', 'CHUNK_SIZE', 'chunk_size')

        r.execute_command('TS.CREATE', 'a')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE', 'a')  # filter exists


def test_create_retention():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', 'tester', 'RETENTION', 1000)

        assert r.execute_command('TS.ADD', 'tester', 500, 10)
        expected_result = [[500, b'10']]
        actual_result = r.execute_command('TS.range', 'tester', '-', '+')
        assert expected_result == actual_result
        # check for (lastTimestamp - retension < 0)
        assert _get_ts_info(r, 'tester').total_samples == 1

        assert r.execute_command('TS.ADD', 'tester', 1001, 20)
        expected_result = [[500, b'10'], [1001, b'20']]
        actual_result = r.execute_command('TS.range', 'tester', '-', '+')
        assert expected_result == actual_result
        assert _get_ts_info(r, 'tester').total_samples == 2

        assert r.execute_command('TS.ADD', 'tester', 2000, 30)
        expected_result = [[1001, b'20'], [2000, b'30']]
        actual_result = r.execute_command('TS.range', 'tester', '-', '+')
        assert expected_result == actual_result
        assert _get_ts_info(r, 'tester').total_samples == 2

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE', 'negative', 'RETENTION', -10)


def test_create_with_negative_chunk_size():
    with Env().getClusterConnectionIfNeeded() as r:
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE', 'tester', 'CHUNK_SIZE', -10)


def test_check_retention_64bit():
    with Env().getClusterConnectionIfNeeded() as r:
        huge_timestamp = 4000000000  # larger than uint32
        r.execute_command('TS.CREATE', 'tester', 'RETENTION', huge_timestamp)
        assert _get_ts_info(r, 'tester').retention_msecs == huge_timestamp
        for i in range(10):
            r.execute_command('TS.ADD', 'tester', int(huge_timestamp * i / 4), i)
        assert r.execute_command('TS.RANGE', 'tester', 0, -1) == \
               [[5000000000, b'5'], [6000000000, b'6'], [7000000000, b'7'],
                [8000000000, b'8'], [9000000000, b'9']]


def test_uncompressed():
    with Env().getClusterConnectionIfNeeded() as r:
        # test simple commands
        r.execute_command('ts.create', 'not_compressed', 'UNCOMPRESSED')
        assert 1 == r.execute_command('ts.add', 'not_compressed', 1, 3.5)
        assert 3.5 == float(r.execute_command('ts.get', 'not_compressed')[1])
        assert 2 == r.execute_command('ts.add', 'not_compressed', 2, 4.5)
        assert 3 == r.execute_command('ts.add', 'not_compressed', 3, 5.5)
        assert 5.5 == float(r.execute_command('ts.get', 'not_compressed')[1])
        assert [[1, b'3.5'], [2, b'4.5'], [3, b'5.5']] == \
               r.execute_command('ts.range', 'not_compressed', 0, -1)
        info = _get_ts_info(r, 'not_compressed')
        assert info.total_samples == 3 and info.memory_usage == 4144

        # rdb load
        data = r.execute_command('dump', 'not_compressed')
        r.execute_command('del', 'not_compressed')

    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command('RESTORE', 'not_compressed', 0, data)
        assert [[1, b'3.5'], [2, b'4.5'], [3, b'5.5']] == \
               r.execute_command('ts.range', 'not_compressed', 0, -1)
        info = _get_ts_info(r, 'not_compressed')
        assert info.total_samples == 3 and info.memory_usage == 4144
        # test deletion
        assert r.delete('not_compressed')


def test_trim():
    with Env().getClusterConnectionIfNeeded() as r:
        for mode in ["UNCOMPRESSED", "COMPRESSED"]:
            samples = 2000
            chunk_size = 64 * SAMPLE_SIZE
            total_chunk_count = math.ceil(float(samples) / float(chunk_size) * SAMPLE_SIZE)
            r.execute_command('ts.create', 'trim_me', 'CHUNK_SIZE', chunk_size, 'RETENTION', 10, mode)
            r.execute_command('ts.create', 'dont_trim_me', 'CHUNK_SIZE', chunk_size, mode)
            for i in range(samples):
                r.execute_command('ts.add', 'trim_me', i, i * 1.1)
                r.execute_command('ts.add', 'dont_trim_me', i, i * 1.1)

            trimmed_info = _get_ts_info(r, 'trim_me')
            untrimmed_info = _get_ts_info(r, 'dont_trim_me')
            assert 2 == trimmed_info.chunk_count
            assert samples == untrimmed_info.total_samples
            # extra test for uncompressed
            if mode == "UNCOMPRESSED":
                assert 11 == trimmed_info.total_samples
                assert total_chunk_count == untrimmed_info.chunk_count

            r.delete("trim_me")
            r.delete("dont_trim_me")


def test_empty():
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command('ts.create', 'empty')
        info = _get_ts_info(r, 'empty')
        assert info.total_samples == 0
        assert [] == r.execute_command('TS.range', 'empty', 0, -1)
        assert [] == r.execute_command('TS.get', 'empty')

        r.execute_command('ts.create', 'empty_uncompressed', 'uncompressed')
        info = _get_ts_info(r, 'empty_uncompressed')
        assert info.total_samples == 0
        assert [] == r.execute_command('TS.range', 'empty_uncompressed', 0, -1)
        assert [] == r.execute_command('TS.get', 'empty')


def test_issue299():
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command('ts.create', 'issue299')
        for i in range(1000):
            r.execute_command('ts.add', 'issue299', i * 10, i)
        actual_result = r.execute_command('ts.range', 'issue299', 0, -1, 'aggregation', 'avg', 10)
        assert actual_result[0] == [0, b'0']
        actual_result = r.execute_command('ts.range', 'issue299', 0, -1, 'aggregation', 'avg', 100)
        assert actual_result[0] == [0, b'4.5']

        r.execute_command('del', 'issue299')
        r.execute_command('ts.create', 'issue299')
        for i in range(100, 1000):
            r.execute_command('ts.add', 'issue299', i * 10, i)
        actual_result = r.execute_command('ts.range', 'issue299', 0, -1, 'aggregation', 'avg', 10)
        assert actual_result[0] != [0, b'0']


def test_expire():
    Env().skipOnCluster()
    with Env().getConnection() as r:
        assert r.execute_command('ts.create', 'test') == b'OK'
        assert r.execute_command('keys', '*') == [b'test']
        assert r.execute_command('expire', 'test', 1) == 1
        time.sleep(2)
        assert r.execute_command('keys', '*') == []
