import pytest
import redis
from RLTest import Env


def test_errors():
    with Env().getConnection() as r:
        # test wrong arity
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATE')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.ALTER')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.ADD')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MADD')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.INCRBY')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.DECRBY')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.CREATERULE')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.DELETERULE')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.QUERYINDEX')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.GET')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MRANGE')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.INFO')

    with Env().getClusterConnectionIfNeeded() as r:
        # different type key
        r.execute_command('SET', 'foo', 'bar')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.GET', 'foo', '*', '5')  # too many args
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.GET', 'foo')  # wrong type
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.GET', 'bar')  # does not exist
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.INFO', 'foo')  # wrong type
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.INFO', 'bar')  # does not exist
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.RANGE', 'foo', '0', '-1')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.ALTER', 'foo')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.INCRBY', 'foo', '1', 'timestamp', '5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.DECRBY', 'foo', '1', 'timestamp', '5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.ADD', 'values', 'timestamp', '5')  # string
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.ADD', 'values', '*', 'value')  # string
        with pytest.raises(redis.ResponseError) as excinfo:
            labels = ["abc"] * 51
            assert r.execute_command('TS.MGET', 'SELECTED_LABELS', *labels, 'FILTER', 'metric=cpu')
        with pytest.raises(redis.ResponseError) as excinfo:
            labels = ["abc"] * 51
            assert r.execute_command('TS.MRANGE', 'SELECTED_LABELS', *labels, 'FILTER', 'metric=cpu')
