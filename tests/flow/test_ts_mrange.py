import pytest
import redis
import time
from RLTest import Env
from test_helper_classes import _insert_data


def test_mrange_with_expire_cmd():
    with Env().getConnection() as r:
        # Lower hz value to make it more likely that mrange triggers key expiration
        assert r.execute_command('config set hz 1') == b'OK'
        assert r.execute_command("TS.ADD", "X" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        assert r.execute_command("TS.ADD", "Y" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        assert r.execute_command("TS.ADD", "Z" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        current_ts = time.time()
        assert r.execute_command("EXPIRE","X", 5)
        assert r.execute_command("EXPIRE","Y", 6)
        assert r.execute_command("EXPIRE","Z", 7)
        while time.time() < (current_ts+10):
            reply = r.execute_command('TS.mrange', '-', '+', 'FILTER', 'type=DELAYED')
            assert(len(reply)>=0 and len(reply)<=3)
        assert r.execute_command("PING")

def test_mrange_expire_issue549():
    Env().skipOnDebugger()
    with Env().getConnection() as r:
        # Lower hz value to make it more likely that mrange triggers key expiration
        assert r.execute_command('config set hz 1') == b'OK'
        assert r.execute_command('ts.add k1 1 10 LABELS l 1') == 1
        assert r.execute_command('ts.add k2 2 20 LABELS l 1') == 2
        assert r.execute_command('expire k1 1') == 1
        for i in range(0, 50000):
            assert r.execute_command('ts.mrange - + aggregation avg 10 withlabels filter l=1') is not None


def test_range_by_labels():
    start_ts = 1511885909
    samples_count = 50

    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
        _insert_data(r, 'tester1', start_ts, samples_count, 5)
        _insert_data(r, 'tester2', start_ts, samples_count, 15)
        _insert_data(r, 'tester3', start_ts, samples_count, 25)

        expected_result = [[start_ts + i, str(5).encode('ascii')] for i in range(samples_count)]
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
        assert [[b'tester1', [], expected_result]] == actual_result
        expected_result.reverse()
        actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
        assert [[b'tester1', [], expected_result]] == actual_result

        def build_expected(val, time_bucket):
            return [[int(i - i % time_bucket), str(val).encode('ascii')] for i in
                    range(start_ts, start_ts + samples_count + 1, time_bucket)]

        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'LAST', 5,
                                          'FILTER', 'generation=x')
        expected_result = [[b'tester1', [], build_expected(5, 5)],
                           [b'tester2', [], build_expected(15, 5)],
                           [b'tester3', [], build_expected(25, 5)],
                           ]
        assert expected_result == actual_result
        assert expected_result[1:] == r.execute_command('TS.mrange', start_ts, start_ts + samples_count,
                                                        'AGGREGATION', 'LAST', 5, 'FILTER', 'generation=x',
                                                        'class!=middle')
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 3, 'AGGREGATION',
                                          'LAST', 5, 'FILTER', 'generation=x')
        assert expected_result[0][2][:3] == actual_result[0][2][:3]
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 5,
                                          'FILTER', 'generation=x')
        assert [[1511885905, b'1']] == actual_result[0][2][:1]
        assert expected_result[0][2][1:9] == actual_result[0][2][1:9]
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 3,
                                          'COUNT', 3, 'FILTER', 'generation=x')
        assert 3 == len(actual_result[0][2])  # just checking that agg count before count works
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 3, 'AGGREGATION',
                                          'COUNT', 3, 'FILTER', 'generation=x')
        assert 3 == len(actual_result[0][2])  # just checking that agg count before count works
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 3,
                                          'FILTER', 'generation=x')
        assert 18 == len(actual_result[0][2])  # just checking that agg count before count works

        actual_result = r.execute_command('TS.mrange', start_ts+2, -1, 'COUNT', 1, 'OFFSET', 2,
                                          'FILTER', 'generation=x')
        assert start_ts + 4==actual_result[0][2][0][0] 

        # Note that we need to have the offset synchronized to the aggregation to allow for consistent offset values.
        actual_result = r.execute_command('TS.mrange', start_ts+2, -1, 'COUNT', 1, 'OFFSET', (start_ts + 5) % 3,
                                          'AGGREGATION', 'COUNT', 3, 'FILTER', 'generation=x')
        assert start_ts + 2 == actual_result[0][2][0][0]

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'invalid', 3,
                                     'FILTER', 'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'AVG', 'string',
                                     'FILTER', 'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 'string', 'FILTER',
                                     'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'OFFSET', 'string', 'FILTER',
                                     'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'OFFSET', 'FILTER','generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange - + FILTER')  # missing args
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange - + RETLIF')  # no filter word
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', 'string', start_ts + samples_count, 'FILTER', 'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, 'string', 'FILTER', 'generation=x')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'generation+x')

        # issue 414
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=(bob,rudy,)')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=(bob,,rudy)')


def test_mrange_withlabels():
    start_ts = 1511885909
    samples_count = 50

    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
        _insert_data(r, 'tester1', start_ts, samples_count, 5)
        _insert_data(r, 'tester2', start_ts, samples_count, 15)
        _insert_data(r, 'tester3', start_ts, samples_count, 25)

        expected_result = [[start_ts + i, str(5).encode('ascii')] for i in range(samples_count)]
        actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'WITHLABELS', 'FILTER',
                                          'name=bob')
        assert [[b'tester1', [[b'name', b'bob'], [b'class', b'middle'], [b'generation', b'x']],
                 expected_result]] == actual_result
        actual_result = r.execute_command('TS.mrange', start_ts + 1, start_ts + samples_count, 'WITHLABELS',
                                          'AGGREGATION', 'COUNT', 1, 'FILTER', 'generation=x')
        # assert the labels length is 3 (name,class,generation) for each of the returned time-series
        assert len(actual_result[0][1]) == 3
        assert len(actual_result[1][1]) == 3
        assert len(actual_result[2][1]) == 3


def test_multilabel_filter():
    with Env().getConnection() as r:
        assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
        assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')

        assert r.execute_command('TS.ADD', 'tester1', 0, 1) == 0
        assert r.execute_command('TS.ADD', 'tester2', 0, 2) == 0
        assert r.execute_command('TS.ADD', 'tester3', 0, 3) == 0

        actual_result = r.execute_command('TS.mrange', 0, -1, 'WITHLABELS', 'FILTER', 'name=(bob,rudy)')
        assert actual_result[0][0] == b'tester1'
        assert actual_result[1][0] == b'tester2'

        actual_result = r.execute_command('TS.mrange', 0, -1, 'WITHLABELS', 'FILTER', 'name=(bob,rudy)',
                                          'class!=(middle,top)')
        assert actual_result[0][0] == b'tester2'

        actual_result = r.execute_command('TS.mget', 'WITHLABELS', 'FILTER', 'name=(bob,rudy)')
        assert actual_result[0][0] == b'tester1'
        assert actual_result[1][0] == b'tester2'

        actual_result = r.execute_command('TS.mget', 'WITHLABELS', 'FILTER', 'name=(bob,rudy)', 'class!=(middle,top)')
        assert actual_result[0][0] == b'tester2'
