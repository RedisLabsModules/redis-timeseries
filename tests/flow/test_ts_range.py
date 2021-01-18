import math

import pytest
import redis
from RLTest import Env
from test_helper_classes import TSInfo, ALLOWED_ERROR, _insert_data, _get_ts_info, \
    _insert_agg_data


def test_range_query():
    start_ts = 1488823384
    samples_count = 1500
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester', 'uncompressed', 'RETENTION', samples_count - 100)
        _insert_data(r, 'tester', start_ts, samples_count, 5)

        expected_result = [[start_ts + i, str(5).encode('ascii')] for i in range(99, 151)]
        actual_result = r.execute_command('TS.range', 'tester', start_ts + 50, start_ts + 150)
        assert expected_result == actual_result
        rev_result = r.execute_command('TS.revrange', 'tester', start_ts + 50, start_ts + 150)
        rev_result.reverse()
        assert expected_result == rev_result

        # test out of range returns empty list
        assert [] == r.execute_command('TS.range', 'tester', int(start_ts * 2), -1)
        assert [] == r.execute_command('TS.range', 'tester', int(start_ts / 3), int(start_ts / 2))

        assert [] == r.execute_command('TS.revrange', 'tester', int(start_ts * 2), -1)
        assert [] == r.execute_command('TS.revrange', 'tester', int(start_ts / 3), int(start_ts / 2))

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester string -1')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 string')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE nonexist 0 -1')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 count number')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 count')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 aggregation count number')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 aggregation count')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 offset number')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE tester 0 -1 offset')


def test_range_midrange():
    samples_count = 5000
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester', 'UNCOMPRESSED')
        for i in range(samples_count):
            r.execute_command('TS.ADD', 'tester', i, i)
        res = r.execute_command('TS.RANGE', 'tester', samples_count - 500, samples_count)
        assert len(res) == 500  # sample_count is not in range() so not included
        res = r.execute_command('TS.RANGE', 'tester', samples_count - 1500, samples_count - 1000)
        assert len(res) == 501

        # test for empty range between two full ranges
        for i in range(samples_count):
            r.execute_command('TS.ADD', 'tester', samples_count * 2 + i, i)
        res = r.execute_command('TS.RANGE', 'tester', int(samples_count * 1.1), int(samples_count + 1.2))
        assert res == []


def test_range_with_agg_query():
    start_ts = 1488823384
    samples_count = 1500
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester')
        _insert_data(r, 'tester', start_ts, samples_count, 5)

        expected_result = [[1488823000, b'116'], [1488823500, b'500'], [1488824000, b'500'], [1488824500, b'384']]
        actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count, 'AGGREGATION',
                                          'count', 500)
        assert expected_result == actual_result

        # test first aggregation is not [0,0] if out of range
        expected_result = [[1488823000, b'116'], [1488823500, b'500']]
        actual_result = r.execute_command('TS.range', 'tester', 1488822000, 1488823999, b'AGGREGATION',
                                          'count', 500)
        assert expected_result == actual_result

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count, 'AGGREGATION',
                                     'count', -1)


def test_agg_std_p():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'std.p')

        expected_result = [[10, b'25.869'], [20, b'25.869'], [30, b'25.869'], [40, b'25.869']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        for i in range(len(expected_result)):
            assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR


def test_agg_std_s():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'std.s')

        expected_result = [[10, b'27.269'], [20, b'27.269'], [30, b'27.269'], [40, b'27.269']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        for i in range(len(expected_result)):
            assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR


def test_agg_var_p():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'var.p')

        expected_result = [[10, b'669.25'], [20, b'669.25'], [30, b'669.25'], [40, b'669.25']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        for i in range(len(expected_result)):
            assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR


def test_agg_var_s():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'var.s')

        expected_result = [[10, b'743.611'], [20, b'743.611'], [30, b'743.611'], [40, b'743.611']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        for i in range(len(expected_result)):
            assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR


def test_agg_sum():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'sum')

        expected_result = [[10, b'1565'], [20, b'2565'], [30, b'3565'], [40, b'4565']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_count():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'count')

        expected_result = [[10, b'10'], [20, b'10'], [30, b'10'], [40, b'10']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_first():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'first')

        expected_result = [[10, b'131'], [20, b'231'], [30, b'331'], [40, b'431']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_last():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'last')

        expected_result = [[10, b'184'], [20, b'284'], [30, b'384'], [40, b'484']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_range():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'range')

        expected_result = [[10, b'74'], [20, b'74'], [30, b'74'], [40, b'74']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_range_count():
    start_ts = 1511885908
    samples_count = 50

    with Env().getConnection() as r:
        r.execute_command('TS.CREATE', 'tester1')
        for i in range(samples_count):
            r.execute_command('TS.ADD', 'tester1', start_ts + i, i)
        full_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
        assert len(full_results) == samples_count
        count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, b'COUNT', 10)
        assert count_results == full_results[:10]
        count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, b'COUNT', 10, b'AGGREGATION', 'COUNT', 3)
        assert len(count_results) == 10
        count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, b'AGGREGATION', 'COUNT', 4, b'COUNT', 10)
        assert len(count_results) == 10
        count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, b'AGGREGATION', 'COUNT', 3)
        assert len(count_results) == math.ceil(samples_count / 3.0)


def test_range_offset():
    start_ts = 10
    samples_count = 10
    key = 'test_offset'

    with Env().getConnection() as r:
        r.execute_command('TS.CREATE', key)
        for i in range(samples_count):
            r.execute_command('TS.ADD', key, start_ts + i, i)
        full_results = r.execute_command('TS.RANGE', key, 0, -1)
        assert len(full_results) == samples_count
        offset_results = r.execute_command('TS.RANGE', key, 0, -1, b'OFFSET', 5)
        assert offset_results == [[i+5, v] for i, v in full_results]
        count_results = r.execute_command('TS.RANGE', key, start_ts + 1, -1, b'OFFSET', 1, b'AGGREGATION', 'COUNT', 2)
        actual_result = [[11, b'1'], [13, b'2'], [15, b'2'], [17, b'2'], [19, b'2']]
        assert count_results == actual_result
        count_results = r.execute_command('TS.RANGE', key, start_ts + 1, -1, b'AGGREGATION', 'COUNT', 2, b'OFFSET', 1)
        assert count_results == actual_result


def test_agg_min():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'min')

        expected_result = [[10, b'123'], [20, b'223'], [30, b'323'], [40, b'423']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_max():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'max')

        expected_result = [[10, b'197'], [20, b'297'], [30, b'397'], [40, b'497']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_agg_avg():
    with Env().getConnection() as r:
        agg_key = _insert_agg_data(r, 'tester', 'avg')

        expected_result = [[10, b'156.5'], [20, b'256.5'], [30, b'356.5'], [40, b'456.5']]
        actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
        assert expected_result == actual_result


def test_series_ordering():
    with Env().getConnection() as r:
        sample_len = 1024
        chunk_size = 4

        r.execute_command("ts.create", 'test_key', 0, chunk_size)
        for i in range(sample_len):
            r.execute_command("ts.add", 'test_key', i, i)

        res = r.execute_command('ts.range', 'test_key', 0, sample_len)
        i = 0
        for sample in res:
            assert sample == [i, str(i).encode('ascii')]
            i += 1


def test_sanity():
    start_ts = 1511885909
    samples_count = 1500
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester', 'RETENTION', '0', 'CHUNK_SIZE', '1024', 'LABELS', 'name',
                                 'brown', 'color', 'pink')
        _insert_data(r, 'tester', start_ts, samples_count, 5)

        expected_result = [[start_ts + i, str(5).encode('ascii')] for i in range(samples_count)]
        actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count)
        assert expected_result == actual_result
        expected_result = [
            b'totalSamples', 1500, b'memoryUsage', 1166,
            b'firstTimestamp', start_ts, b'chunkCount', 1,
            b'labels', [[b'name', b'brown'], [b'color', b'pink']],
            b'lastTimestamp', start_ts + samples_count - 1,
            b'chunkSize', 1024, b'retentionTime', 0,
            b'sourceKey', None, b'rules', []]
        assert TSInfo(expected_result) == _get_ts_info(r, 'tester')


def test_sanity_pipeline():
    start_ts = 1488823384
    samples_count = 1500
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester')
        with r.pipeline(transaction=False) as p:
            p.set("name", "danni")
            _insert_data(p, 'tester', start_ts, samples_count, 5)
            p.execute()
        expected_result = [[start_ts + i, str(5).encode('ascii')] for i in range(samples_count)]
        actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count)
        assert expected_result == actual_result


def test_issue358():
    filepath = "./issue358.txt"
    with Env().getConnection() as r:
        r.execute_command('ts.create issue358')

        with open(filepath) as fp:
            line = fp.readline()
            while line:
                line = fp.readline()
                if line != '':
                    r.execute_command(line)
        range_res = r.execute_command('ts.range issue358', 1582848000, -1)[0][1]
        get_res = r.execute_command('ts.get issue358')[1]
        assert range_res == get_res
