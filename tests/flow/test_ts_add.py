import time

import pytest
import redis
from RLTest import Env
from test_helper_classes import _get_ts_info

CHUNK_TYPES = ["COMPRESSED:TURBO_GORILLA","","COMPRESSED","COMPRESSED:GORILLA", "UNCOMPRESSED"]

def test_issue_504():
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command('ts.create', 'tester')
        for i in range(100, 3000):
            assert r.execute_command('ts.add', 'tester', i, i * 1.1) == i
        assert r.execute_command('ts.add', 'tester', 99, 1) == 99
        assert r.execute_command('ts.add', 'tester', 98, 1) == 98


def test_issue_588():
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command('ts.create', 'test1', "DUPLICATE_POLICY", "min")
        r.execute_command('ts.add', 'test1', 1, -0.05)
        assert float(r.execute_command('TS.RANGE', 'test1', "-", "+")[0][1]) == -0.05
        r.execute_command('ts.add', 'test1', 1, -0.06)
        assert float(r.execute_command('TS.RANGE', 'test1', "-", "+")[0][1]) == -0.06

        r.execute_command('ts.create', 'test2', "DUPLICATE_POLICY", "max")
        r.execute_command('ts.add', 'test2', 1, -0.06)
        assert float(r.execute_command('TS.RANGE', 'test2', "-", "+")[0][1]) == -0.06
        r.execute_command('ts.add', 'test2', 1, -0.05)
        assert float(r.execute_command('TS.RANGE', 'test2', "-", "+")[0][1]) == -0.05


def test_automatic_timestamp():
    with Env().getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.CREATE', 'tester')
        response_timestamp = r.execute_command('TS.ADD', 'tester', '*', 1)
        curr_time = int(time.time() * 1000)
        result = r.execute_command('TS.RANGE', 'tester', 0, curr_time)
        # test time difference is not more than 5 milliseconds
        assert result[0][0] - curr_time <= 5
        assert response_timestamp - curr_time <= 5


def test_add_create_key():
    with Env().getClusterConnectionIfNeeded() as r:
        ts = time.time()
        assert r.execute_command('TS.ADD', 'tester1', str(int(ts)), str(ts), 'RETENTION', '666', 'LABELS', 'name',
                                 'blabla') == int(ts)
        info = _get_ts_info(r, 'tester1')
        assert info.total_samples == 1
        assert info.retention_msecs == 666
        assert info.labels == {b'name': b'blabla'}

        assert r.execute_command('TS.ADD', 'tester2', str(int(ts)), str(ts), 'LABELS', 'name', 'blabla2', 'location',
                                 'earth')
        info = _get_ts_info(r, 'tester2')
        assert info.total_samples == 1
        assert info.labels == {b'location': b'earth', b'name': b'blabla2'}


def test_valid_labels():
    with Env().getClusterConnectionIfNeeded() as r:
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.CREATE', 'tester', 'LABELS', 'name', '')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'tester2', '*', 1, 'LABELS', 'name', 'myName', 'location', '')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'tester2', '*', 1, 'LABELS', 'name', 'myName', 'location', 'list)')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'tester2', '*', 1, 'LABELS', 'name', 'myName', 'location', 'li(st')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'tester2', '*', 1, 'LABELS', 'name', 'myName', 'location', 'lis,t')           
            
def test_valid_timestamp():
    with Env().getConnection() as r:
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'timestamp', '12434fd', '34')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'timestamp', '-34', '22')
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command('TS.ADD', 'timestamp', '*235', '45')

def test_chunk_types():
    for CHUNK_TYPE in CHUNK_TYPES:
        e = Env()
        with e.getClusterConnectionIfNeeded() as r:
            r.execute_command('ts.create', 'monkey', CHUNK_TYPE)
            r.execute_command('ts.add', 'monkey', '0', '1')
            r.execute_command('ts.add', 'monkey', '1', '1')
            r.execute_command('ts.add', 'monkey', '2', '1')
            r.execute_command('ts.add', 'monkey', '50', '1')
            r.execute_command('ts.add', 'monkey', '51', '1')
            r.execute_command('ts.add', 'monkey', '500', '1')
            r.execute_command('ts.add', 'monkey', '501', '1')
            r.execute_command('ts.add', 'monkey', '3000', '1')
            r.execute_command('ts.add', 'monkey', '3001', '1')
            r.execute_command('ts.add', 'monkey', '10000', '1')
            r.execute_command('ts.add', 'monkey', '10001', '1')
            r.execute_command('ts.add', 'monkey', '100000', '1')
            r.execute_command('ts.add', 'monkey', '100001', '1')
            r.execute_command('ts.add', 'monkey', '100002', '1')
            r.execute_command('ts.add', 'monkey', '100004', '1')
            r.execute_command('ts.add', 'monkey', '1000000', '1')
            r.execute_command('ts.add', 'monkey', '1000001', '1')
            r.execute_command('ts.add', 'monkey', '10000011000001', '1')
            r.execute_command('ts.add', 'monkey', '10000011000002', '1')
            expected_result = [[0, b'1'], [1, b'1'], [2, b'1'], [50, b'1'], [51, b'1'],
                            [500, b'1'], [501, b'1'], [3000, b'1'], [3001, b'1'],
                            [10000, b'1'], [10001, b'1'], [100000, b'1'], [100001, b'1'],
                            [100002, b'1'], [100004, b'1'], [1000000, b'1'], [1000001, b'1'],
                            [10000011000001, b'1'], [10000011000002, b'1']]
            assert expected_result == r.execute_command('TS.range', 'monkey', 0, -1)
        e.flush()


def test_extensive_ts_add():
    for CHUNK_TYPE in CHUNK_TYPES:
        e = Env(decodeResponses=True)
        e.flush()
        with e.getClusterConnectionIfNeeded() as r:
            r.execute_command("ts.create", 'test_key1', CHUNK_TYPE)
            pos = 1
            lines = []
            float_lines = []
            with open("lemire_canada.txt","r") as file:
                lines = file.readlines()
            for line in lines:
                float_v = float(line.strip())
                res = r.execute_command("ts.add", 'test_key1', pos, float_v)
                assert res == pos
                pos=pos+1
                float_lines.append(float_v)
            returned_floats = r.execute_command('ts.range', 'test_key1', "-", "+")
            assert len(returned_floats) == len(float_lines)
            for pos,datapoint in enumerate(returned_floats,start=1):
                assert pos == datapoint[0]
                assert float_lines[pos-1] == float(datapoint[1])
