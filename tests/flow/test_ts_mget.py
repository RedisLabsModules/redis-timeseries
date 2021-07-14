import pytest
import redis
import time 
from utils import Env, set_hertz

def test_mget_with_expire_cmd():
    set_hertz(Env())
    with Env().getClusterConnectionIfNeeded() as r:
        # Lower hz value to make it more likely that mget triggers key expiration
        assert r.execute_command("TS.ADD", "X" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        assert r.execute_command("TS.ADD", "Y" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        assert r.execute_command("TS.ADD", "Z" ,"*" ,"1" ,"LABELS", "type", "DELAYED")
        current_ts = time.time()
        assert r.execute_command("EXPIRE","X", 5)
        assert r.execute_command("EXPIRE","Y", 6)
        assert r.execute_command("EXPIRE","Z", 7)
        while time.time() < (current_ts+10):
            reply = r.execute_command('TS.MGET', 'FILTER', 'type=DELAYED')
            assert(len(reply)>=0 and len(reply)<=3)
        assert r.execute_command("PING")
        

def test_mget_cmd():
    num_of_keys = 3
    time_stamp = 1511885909
    keys = ['k1', 'k2', 'k3']
    labels = ['a', 'a', 'b']
    values = [100, 200, 300]
    kvlabels = []

    with Env(decodeResponses=True).getClusterConnectionIfNeeded() as r:
        # test for empty series
        assert r.execute_command('TS.CREATE', "key4_empty", "LABELS", "NODATA", "TRUE")
        assert r.execute_command('TS.CREATE', "key5_empty", "LABELS", "NODATA", "TRUE")
        # expect to received time-series k1 and k2
        expected_result = [
            ["key4_empty", [], []],
            ["key5_empty", [], []]
        ]

        actual_result = r.execute_command('TS.MGET', 'FILTER', 'NODATA=TRUE')
        assert sorted(expected_result) == sorted(actual_result)

        # test for series with data
        for i in range(num_of_keys):
            assert r.execute_command('TS.CREATE', keys[i], 'LABELS', labels[i], '1', 'metric', 'cpu')
            kvlabels.append([[labels[i], '1'], ['metric', 'cpu']])

            assert r.execute_command('TS.ADD', keys[i], time_stamp - 1, values[i] - 1)
            assert r.execute_command('TS.ADD', keys[i], time_stamp, values[i])

        # expect to received time-series k1 and k2
        expected_result = [
            [keys[0], [], [time_stamp, str(values[0])]],
            [keys[1], [], [time_stamp, str(values[1])]]
        ]

        actual_result = r.execute_command('TS.MGET', 'FILTER', 'a=1')
        assert sorted(expected_result) == sorted(actual_result)

        # expect to received time-series k3 with labels
        expected_result_withlabels = [
            [keys[2], kvlabels[2], [time_stamp, str(values[2])]]
        ]

        actual_result = r.execute_command('TS.MGET', 'WITHLABELS', 'FILTER', 'a!=1', 'b=1')
        assert expected_result_withlabels == actual_result

        # expect to received time-series k1 and k2 with labels
        expected_result_withlabels = [
            [keys[0], kvlabels[0], [time_stamp, str(values[0])]],
            [keys[1], kvlabels[1], [time_stamp, str(values[1])]]
        ]

        actual_result = r.execute_command('TS.MGET', 'WITHLABELS', 'FILTER', 'a=1')
        assert sorted(expected_result_withlabels) == sorted(actual_result)

        # expect to recieve only some labels
        expected_labels = [["metric", "cpu"], ["new_label", None]]
        expected_result_withlabels = [
            [keys[0], expected_labels, [time_stamp, str(values[0])]],
            [keys[1], expected_labels, [time_stamp, str(values[1])]]
        ]

        actual_result = r.execute_command('TS.MGET', 'SELECTED_LABELS', 'metric', "new_label",'FILTER', 'a=1')
        assert sorted(expected_result_withlabels) == sorted(actual_result)

        # negative test
        assert not r.execute_command('TS.MGET', 'FILTER', 'a=100')
        assert not r.execute_command('TS.MGET', 'FILTER', 'k=1')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'filter')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'filter', 'k+1')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'filter', 'k!=5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'retlif', 'k!=5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'SELECTED_LABELS', 'filter', 'k!=5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'SELECTED_LABELS', 'filter', 'k!=5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'SELECTED_LABELS', 'WITHLABELS', 'filter', 'k!=5')
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.MGET', 'WITHLABELS', 'SELECTED_LABELS', 'filter', 'k!=5')

def test_large_key_value_pairs():
     with Env().getClusterConnectionIfNeeded() as r:
        number_series = 100
        for i in range(0,number_series):
            assert r.execute_command('TS.CREATE', 'ts-{}'.format(i), 'LABELS', 'baseAsset', '17049', 'counterAsset', '840', 'source', '1000', 'dataType', 'PRICE_TICK')

        kv_label1 = 'baseAsset=(13830,10249,16019,10135,17049,10777,10138,11036,11292,15778,11043,10025,11436,12207,13359,10807,12216,11833,10170,10811,12864,12738,10053,11334,12487,12619,12364,13266,11219,15827,12374,11223,10071,12249,11097,14430,13282,16226,13667,11365,12261,12646,12650,12397,12785,13941,10231,16254,12159,15103)'
        kv_label2 = 'counterAsset=(840)'
        kv_label3 = 'source=(1000)'
        kv_label4 = 'dataType=(PRICE_TICK)'
        kv_labels = [kv_label1, kv_label2, kv_label3, kv_label4]
        for kv_label in kv_labels:
            res = r.execute_command('TS.MGET', 'FILTER', kv_label1)
            assert len(res) == number_series
