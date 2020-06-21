import os
import redis
import pytest
import time
import __builtin__
import math
import random
import statistics 
from rmtest import ModuleTestCase
from includes import *

if os.environ['REDISTIMESERIES'] != '':
    REDISTIMESERIES = os.environ['REDISTIMESERIES']
else:
    REDISTIMESERIES = os.path.dirname(os.path.abspath(__file__)) + '/redistimeseries.so'

ALLOWED_ERROR = 0.001

def list_to_dict(aList):
    return {aList[i][0]:aList[i][1] for i in range(len(aList))}

class TSInfo(object):
    rules = []
    labels = []
    sourceKey = None
    chunk_count = None
    memory_usage = None
    total_samples = None
    retention_msecs = None
    last_time_stamp = None
    first_time_stamp = None
    max_samples_per_chunk = None

    def __init__(self, args):
        response = dict(zip(args[::2], args[1::2]))
        if 'rules' in response: self.rules = response['rules']
        if 'sourceKey' in response: self.sourceKey = response['sourceKey']
        if 'chunkCount' in response: self.chunk_count = response['chunkCount']
        if 'labels' in response: self.labels = list_to_dict(response['labels'])
        if 'memoryUsage' in response: self.memory_usage = response['memoryUsage']
        if 'totalSamples' in response: self.total_samples = response['totalSamples']
        if 'retentionTime' in response: self.retention_msecs = response['retentionTime']
        if 'lastTimestamp' in response: self.last_time_stamp = response['lastTimestamp']
        if 'firstTimestamp' in response: self.first_time_stamp = response['firstTimestamp']
        if 'maxSamplesPerChunk' in response: self.max_samples_per_chunk = response['maxSamplesPerChunk']

    def __eq__(self, other): 
        if not isinstance(other, TSInfo):
            return NotImplemented
        return self.rules == other.rules and \
        self.sourceKey == other.sourceKey and \
        self.chunk_count == other.chunk_count and \
        self.memory_usage == other.memory_usage and \
        self.total_samples == other.total_samples and \
        self.retention_msecs == other.retention_msecs and \
        self.last_time_stamp == other.last_time_stamp and \
        self.first_time_stamp == other.first_time_stamp and \
        self.max_samples_per_chunk == other.max_samples_per_chunk
        
class RedisTimeseriesTests(ModuleTestCase(REDISTIMESERIES)):
    def _get_ts_info(self, redis, key):
        return TSInfo(redis.execute_command('TS.INFO', key))

    def _assert_alter_cmd(self, r, key, start_ts, end_ts,
                          expected_data=None,
                          expected_retention=None,
                          expected_chunk_size=None,
                          expected_labels=None):
        """
        Test modifications didn't change the data
        :param r: Redis instance
        :param key: redis key
        :param start_ts:
        :param end_ts:
        :param expected_data:
        :return:
        """

        actual_result = self._get_ts_info(r, key)

        if expected_data:
            actual_data = r.execute_command('TS.range', key, start_ts, end_ts)
            assert expected_data == actual_data
        if expected_retention:
            assert expected_retention == actual_result.retention_msecs
        if expected_labels:
            assert list_to_dict(expected_labels) == actual_result.labels
        if expected_chunk_size:
            assert expected_chunk_size == actual_result.max_samples_per_chunk

    @staticmethod
    def _ts_alter_cmd(r, key, set_retention=None, set_chunk_size=None, set_labels=None):
        """
        Assert that changed data is the same as expected, with or without modification of label or retention.
        :param r: Redis instance
        :param key: redis key to work on
        :param set_retention: If none will skip (and test that didn't change from expected result)
        :param set_labels: Format is list. If none will skip (and test that didn't change from expected result)
        :return:
        """
        cmd = ['TS.ALTER', key]
        if set_retention:
            cmd.extend(['RETENTION', set_retention])
        if set_chunk_size:
            cmd.extend(['CHUNK_SIZE', set_chunk_size])
        if set_labels:
            new_labels = [item for sublist in set_labels for item in sublist]
            cmd.extend(['LABELS'])
            cmd.extend(new_labels)
        r.execute_command(*cmd)

    @staticmethod
    def _insert_data(redis, key, start_ts, samples_count, value):
        """
        insert data to key, starting from start_ts, with 1 sec interval between them
        :param redis: redis connection
        :param key: name of time_series
        :param start_ts: beginning of time series
        :param samples_count: number of samples
        :param value: could be a list of samples_count values, or one value. if a list, insert the values in their
        order, if not, insert the single value for all the timestamps
        """
        for i in range(samples_count):
            value_to_insert = value[i] if type(value) == list else value
            actual_result = redis.execute_command('TS.ADD', key, start_ts + i, value_to_insert)
            if type(actual_result) == long:
                assert actual_result == long(start_ts + i)
            else:
                assert actual_result

    def _insert_agg_data(self, redis, key, agg_type):
        agg_key = '%s_agg_%s_10' % (key, agg_type)

        assert redis.execute_command('TS.CREATE', key)
        assert redis.execute_command('TS.CREATE', agg_key)
        assert redis.execute_command('TS.CREATERULE', key, agg_key, "AGGREGATION", agg_type, 10)

        values = (31, 41, 59, 26, 53, 58, 97, 93, 23, 84)
        for i in range(10, 50):
            assert redis.execute_command('TS.ADD', key, i, i // 10 * 100 + values[i % 10])
        # close last bucket
        assert redis.execute_command('TS.ADD', key, 100, 0)

        return agg_key

    @staticmethod
    def _get_series_value(ts_key_result):
        """
        Get only the values from the time stamp series
        :param ts_key_result: the output of ts.range command (pairs of timestamp and value)
        :return: float values of all the values in the series
        """
        return [float(value[1]) for value in ts_key_result]

    @staticmethod
    def _calc_downsampling_series(values, bucket_size, calc_func):
        """
        calculate the downsampling series given the wanted calc_func, by applying to calc_func to all the full buckets
        and to the remainder bucket
        :param values: values of original series
        :param bucket_size: bucket size for downsampling
        :param calc_func: function that calculates the wanted rule, for example min/sum/avg
        :return: the values of the series after downsampling
        """
        series = []
        for i in range(0, int(math.ceil(len(values) / float(bucket_size)))):
            curr_bucket_size = bucket_size
            # we don't have enough values for a full bucket anymore
            if (i + 1) * bucket_size > len(values):
                curr_bucket_size = len(values) - i
            series.append(calc_func(values[i * bucket_size: i * bucket_size + curr_bucket_size]))
        return series

    def calc_rule(self, rule, values, bucket_size):
        """
        Calculate the downsampling with the given rule
        :param rule: 'avg' / 'max' / 'min' / 'sum' / 'count'
        :param values: original series values
        :param bucket_size: bucket size for downsampling
        :return: the values of the series after downsampling
        """
        if rule == 'avg':
            return self._calc_downsampling_series(values, bucket_size, lambda x: float(sum(x)) / len(x))
        elif rule in ['sum', 'max', 'min']:
            return self._calc_downsampling_series(values, bucket_size, getattr(__builtin__, rule))
        elif rule == 'count':
            return self._calc_downsampling_series(values, bucket_size, len)

    def test_sanity(self):
        start_ts = 1511885909L
        samples_count = 1500
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'RETENTION', '0', 'CHUNK_SIZE', '360', 'LABELS', 'name',
                                     'brown', 'color', 'pink')
            self._insert_data(r, 'tester', start_ts, samples_count, 5)

            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count)
            assert expected_result == actual_result

            expected_result = [
              'totalSamples', 1500L, 'memoryUsage', 5902L,
              'firstTimestamp', start_ts, 'chunkCount', 1L,
              'labels', [['name', 'brown'], ['color', 'pink']],
              'lastTimestamp', start_ts + samples_count - 1,
              'maxSamplesPerChunk', 360L, 'retentionTime', 0L,
              'sourceKey', None, 'rules', []]
            assert TSInfo(expected_result) == self._get_ts_info(r, 'tester')

    def test_create_params(self):
        with self.redis() as r:
            # test string instead of value
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATE invalid RETENTION retention')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATE invalid CHUNK_SIZE chunk_size')

            r.execute_command('TS.CREATE a')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATE a') # filter exists

    def test_errors(self):
        with self.redis() as r:
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

            # different type key
            r.execute_command('SET foo bar')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.GET foo * 5') # too many args
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.GET foo') # wrong type
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.GET bar') # does not exist
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.INFO foo') # wrong type
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.INFO bar') # does not exist
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.RANGE foo 0 -1')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.ALTER foo')

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.ADD values timestamp 5')   # string
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.ADD values * value')       # string

    def test_rdb(self):
        start_ts = 1511885909L
        samples_count = 1500
        data = None
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'RETENTION', '0', 'CHUNK_SIZE', '360', 'LABELS', 'name', 'brown', 'color', 'pink')
            assert r.execute_command('TS.CREATE', 'tester_agg_avg_10')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATE', 'tester_agg_sum_10')
            assert r.execute_command('TS.CREATE', 'tester_agg_stds_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_avg_10', 'AGGREGATION', 'AVG', 10)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_sum_10', 'AGGREGATION', 'SUM', 10)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_stds_10', 'AGGREGATION', 'STD.S', 10)
            self._insert_data(r, 'tester', start_ts, samples_count, 5)
            data = r.execute_command('DUMP', 'tester')
            r.execute_command('DEL', 'tester')
            r.execute_command('RESTORE', 'tester', 0, data)
            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count)
            assert expected_result == actual_result
            actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count, 'count', 3)
            assert expected_result[:3] == actual_result

            assert self._get_ts_info(r, 'tester').rules == [['tester_agg_avg_10', 10L, 'AVG'], 
                                                            ['tester_agg_max_10', 10L, 'MAX'],
                                                            ['tester_agg_sum_10', 10L, 'SUM'],
                                                            ['tester_agg_stds_10',10L, 'STD.S']]

    def test_rdb_aggregation_context(self):
        """
        Check that the aggregation context of the rules is saved in rdb. Write data with not a full bucket,
        then save it and restore, add more data to the bucket and check the rules results considered the previous data
        that was in that bucket in their calculation. Check on avg and min, since all the other rules use the same
        context as min.
        """
        start_ts = 3
        samples_count = 4  # 1 full bucket and another one with 1 value
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester_agg_avg_3')
            assert r.execute_command('TS.CREATE', 'tester_agg_min_3')
            assert r.execute_command('TS.CREATE', 'tester_agg_sum_3')
            assert r.execute_command('TS.CREATE', 'tester_agg_std_3')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_avg_3', 'AGGREGATION', 'AVG', 3)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_min_3', 'AGGREGATION', 'MIN', 3)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_sum_3', 'AGGREGATION', 'SUM', 3)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_std_3', 'AGGREGATION', 'STD.S', 3)
            self._insert_data(r, 'tester', start_ts, samples_count, range(samples_count))
            data_tester = r.execute_command('dump', 'tester')
            data_avg_tester = r.execute_command('dump', 'tester_agg_avg_3')
            data_min_tester = r.execute_command('dump', 'tester_agg_min_3')
            data_sum_tester = r.execute_command('dump', 'tester_agg_sum_3')
            data_std_tester = r.execute_command('dump', 'tester_agg_std_3')
            r.execute_command('DEL','tester','tester_agg_avg_3','tester_agg_min_3','tester_agg_sum_3','tester_agg_std_3')
            r.execute_command('RESTORE', 'tester', 0, data_tester)
            r.execute_command('RESTORE', 'tester_agg_avg_3', 0, data_avg_tester)
            r.execute_command('RESTORE', 'tester_agg_min_3', 0, data_min_tester)
            r.execute_command('RESTORE', 'tester_agg_sum_3', 0, data_sum_tester)
            r.execute_command('RESTORE', 'tester_agg_std_3', 0, data_std_tester)
            assert r.execute_command('TS.ADD', 'tester', start_ts + samples_count, samples_count)
            assert r.execute_command('TS.ADD', 'tester', start_ts + samples_count + 10, 0) #closes the last time_bucket
            # if the aggregation context wasn't saved, the results were considering only the new value added
            expected_result_avg = [[start_ts, '1'], [start_ts + 3, '3.5']]
            expected_result_min = [[start_ts, '0'], [start_ts + 3, '3']]
            expected_result_sum = [[start_ts, '3'], [start_ts + 3, '7']]
            expected_result_std = [[start_ts, '1'], [start_ts + 3, '0.7071']]
            actual_result_avg = r.execute_command('TS.range', 'tester_agg_avg_3', start_ts, start_ts + samples_count)
            assert actual_result_avg == expected_result_avg
            actual_result_min = r.execute_command('TS.range', 'tester_agg_min_3', start_ts, start_ts + samples_count)
            assert actual_result_min == expected_result_min
            actual_result_sum = r.execute_command('TS.range', 'tester_agg_sum_3', start_ts, start_ts + samples_count)
            assert actual_result_sum == expected_result_sum
            actual_result_std = r.execute_command('TS.range', 'tester_agg_std_3', start_ts, start_ts + samples_count)
            assert actual_result_std[0] == expected_result_std[0]
            assert abs(float(actual_result_std[1][1]) - float(expected_result_std[1][1])) < ALLOWED_ERROR

    def test_sanity_pipeline(self):
        start_ts = 1488823384L
        samples_count = 1500
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            with r.pipeline(transaction=False) as p:
                p.set("name", "danni")
                self._insert_data(p, 'tester', start_ts, samples_count, 5)
                p.execute()
            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count)
            assert expected_result == actual_result

    def test_alter_cmd(self):
        start_ts = 1511885909L
        samples_count = 1500
        end_ts = start_ts + samples_count
        key = 'tester'

        with self.redis() as r:
            assert r.execute_command('TS.CREATE', key, 'CHUNK_SIZE', '360',
                                     'LABELS', 'name', 'brown', 'color', 'pink')
            self._insert_data(r, key, start_ts, samples_count, 5)

            expected_data = [[start_ts + i, str(5)] for i in range(samples_count)]

            # test alter retention, chunk size and labels
            expected_labels = [['A', '1'], ['B', '2'], ['C', '3']]
            expected_retention = 500
            expected_chunk_size = 100
            self._ts_alter_cmd(r, key, expected_retention, expected_chunk_size, expected_labels)
            self._assert_alter_cmd(r, key, end_ts-501, end_ts, expected_data[-501:], expected_retention,
                                   expected_chunk_size, expected_labels)

            # test alter retention
            expected_retention = 200
            self._ts_alter_cmd(r, key, set_retention=expected_retention)
            self._assert_alter_cmd(r, key, end_ts-201, end_ts, expected_data[-201:], expected_retention,
                                   expected_chunk_size, expected_labels)
            
            # test alter chunk size
            expected_chunk_size = 100
            expected_labels = [['A', '1'], ['B', '2'], ['C', '3']]
            self._ts_alter_cmd(r, key, set_chunk_size=expected_chunk_size)
            self._assert_alter_cmd(r, key, end_ts-201, end_ts, expected_data[-201:], expected_retention,
                                   expected_chunk_size, expected_labels)

            # test alter labels
            expected_labels = [['A', '1']]
            self._ts_alter_cmd(r, key, expected_retention, set_labels=expected_labels)
            self._assert_alter_cmd(r, key, end_ts-201, end_ts, expected_data[-201:], expected_retention,
                                   expected_chunk_size, expected_labels)

            # test indexer was updated
            assert r.execute_command('TS.QUERYINDEX', 'A=1') == [key]
            assert r.execute_command('TS.QUERYINDEX', 'name=brown') == []

    def test_mget_cmd(self):
        num_of_keys = 3
        time_stamp = 1511885909L
        keys = ['k1', 'k2', 'k3']
        labels = ['a', 'a', 'b']
        values = [100, 200, 300]
        kvlabels = []

        with self.redis() as r:
            # test for empty series
            assert r.execute_command('TS.CREATE', "key4_empty", "LABELS", "NODATA", "TRUE")
            assert r.execute_command('TS.CREATE', "key5_empty", "LABELS", "NODATA", "TRUE")
            # expect to received time-series k1 and k2
            expected_result = [
                ["key4_empty", [], []],
                ["key5_empty", [], []]
            ]

            actual_result = r.execute_command('TS.MGET', 'FILTER', 'NODATA=TRUE')
            assert expected_result == actual_result

            # test for series with data
            for i in range(num_of_keys):
                assert r.execute_command('TS.CREATE', keys[i], 'LABELS', labels[i], '1')
                kvlabels.append([labels[i], '1'])

                assert r.execute_command('TS.ADD', keys[i], time_stamp - 1, values[i] - 1)
                assert r.execute_command('TS.ADD', keys[i], time_stamp, values[i])

            # expect to received time-series k1 and k2
            expected_result = [
                [keys[0], [], [time_stamp, str(values[0])]],
                [keys[1], [], [time_stamp, str(values[1])]]
            ]

            actual_result = r.execute_command('TS.MGET', 'FILTER', 'a=1')
            assert expected_result == actual_result

            # expect to received time-series k3 with labels
            expected_result_withlabels = [
                [keys[2], [kvlabels[2]], [time_stamp, str(values[2])]]
            ]
            
            actual_result = r.execute_command('TS.MGET', 'WITHLABELS', 'FILTER', 'a!=1', 'b=1')
            assert expected_result_withlabels == actual_result

            # expect to received time-series k1 and k2 with labels
            expected_result_withlabels = [
                [keys[0], [kvlabels[0]], [time_stamp, str(values[0])]],
                [keys[1], [kvlabels[1]], [time_stamp, str(values[1])]]
            ]
            
            actual_result = r.execute_command('TS.MGET', 'WITHLABELS', 'FILTER', 'a=1')
            assert expected_result_withlabels == actual_result

            # negative test
            assert not r.execute_command('TS.MGET', 'FILTER', 'a=100')
            assert not r.execute_command('TS.MGET', 'FILTER', 'k=1')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.MGET filter')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.MGET filter k+1')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.MGET retlif k!=5')

    def test_range_query(self):
        start_ts = 1488823384L
        samples_count = 1500
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'uncompressed', 'RETENTION', samples_count-100)
            self._insert_data(r, 'tester', start_ts, samples_count, 5)

            expected_result = [[start_ts+i, str(5)] for i in range(99, 151)]
            actual_result = r.execute_command('TS.range', 'tester', start_ts+50, start_ts+150)
            assert expected_result == actual_result
            rev_result = r.execute_command('TS.revrange', 'tester', start_ts+50, start_ts+150)
            rev_result.reverse()
            assert expected_result == rev_result

            #test out of range returns empty list
            assert [] == r.execute_command('TS.range', 'tester', start_ts * 2, -1)
            assert [] == r.execute_command('TS.range', 'tester', start_ts / 3, start_ts / 2)

            assert [] == r.execute_command('TS.revrange', 'tester', start_ts * 2, -1)
            assert [] == r.execute_command('TS.revrange', 'tester', start_ts / 3, start_ts / 2)
            
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

    def test_range_midrange(self):
        samples_count = 5000
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'UNCOMPRESSED')
            for i in range(samples_count):
                r.execute_command('TS.ADD', 'tester', i, i)
            res = r.execute_command('TS.RANGE', 'tester', samples_count - 500, samples_count)
            assert len(res) == 500 #sample_count is not in range() so not included
            res = r.execute_command('TS.RANGE', 'tester', samples_count - 1500, samples_count - 1000)
            assert len(res) == 501

            # test for empty range between two full ranges
            for i in range(samples_count):
                r.execute_command('TS.ADD', 'tester', samples_count * 2 + i, i)
            res = r.execute_command('TS.RANGE', 'tester', int(samples_count * 1.1), int(samples_count + 1.2))
            assert res == []

    def test_range_with_agg_query(self):
        start_ts = 1488823384L
        samples_count = 1500
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            self._insert_data(r, 'tester', start_ts, samples_count, 5)
            
            expected_result = [[1488823000L, '116'], [1488823500L, '500'], [1488824000L, '500'], [1488824500L, '384']]
            actual_result = r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count, 'AGGREGATION',
                                              'count', 500)
            assert expected_result == actual_result

            # test first aggregation is not [0,0] if out of range
            expected_result = [[1488823000L, '116'], [1488823500L, '500']]
            actual_result = r.execute_command('TS.range', 'tester', 1488822000L, 1488823999L, 'AGGREGATION',
                                              'count', 500)
            assert expected_result == actual_result

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.range', 'tester', start_ts, start_ts + samples_count, 'AGGREGATION',
                                              'count', -1)

    def test_compaction_rules(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'CHUNK_SIZE', '360')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', -10)
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', 0)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', 10)

            start_ts = 1488823384L
            samples_count = 1500
            self._insert_data(r, 'tester', start_ts, samples_count, 5)
            last_ts = start_ts + samples_count + 10
            r.execute_command('TS.ADD', 'tester', last_ts, 5)

            actual_result = r.execute_command('TS.RANGE', 'tester_agg_max_10', start_ts, start_ts + samples_count)

            assert len(actual_result) == samples_count/10

            info = self._get_ts_info(r, 'tester')
            assert info.rules == [['tester_agg_max_10', 10L, 'AVG']]

    def test_delete_key(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'CHUNK_SIZE', '360')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', 10)
            assert r.delete('tester_agg_max_10')
            assert self._get_ts_info(r, 'tester').rules == []
            
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', 11)
            assert r.delete('tester')
            assert self._get_ts_info(r, 'tester_agg_max_10').sourceKey == None
            
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'avg', 12)
            assert self._get_ts_info(r, 'tester').rules == [['tester_agg_max_10', 12L, 'AVG']]

    def test_create_retention(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester', 'RETENTION', 1000)
            
            assert r.execute_command('TS.ADD', 'tester', 500, 10)
            expected_result = [[500L, '10']]
            actual_result = r.execute_command('TS.range', 'tester', '-', '+')
            assert expected_result == actual_result
            
            assert r.execute_command('TS.ADD', 'tester', 1001, 20)
            expected_result = [[500L, '10'], [1001L, '20']]
            actual_result = r.execute_command('TS.range', 'tester', '-', '+')
            assert expected_result == actual_result
            
            assert r.execute_command('TS.ADD', 'tester', 2000, 30)
            expected_result = [[1001L, '20'], [2000L, '30']]
            actual_result = r.execute_command('TS.range', 'tester', '-', '+')
            assert expected_result == actual_result

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATE', 'negative', 'RETENTION', -10)

    def test_create_with_negative_chunk_size(self):
        with self.redis() as r:
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATE', 'tester', 'CHUNK_SIZE', -10)

    def test_check_retention_64bit(self):
        with self.redis() as r:

            huge_timestamp = 4000000000 # larger than uint32
            r.execute_command('TS.CREATE', 'tester', 'RETENTION', huge_timestamp)
            assert self._get_ts_info(r, 'tester').retention_msecs == huge_timestamp
            for i in range(10):
                r.execute_command('TS.ADD', 'tester', huge_timestamp * i / 4, i)
            assert r.execute_command('TS.RANGE', 'tester', 0, -1) == \
                [[5000000000L, '5'], [6000000000L, '6'], [7000000000L, '7'],
                 [8000000000L, '8'], [9000000000L, '9']]

    def test_create_compaction_rule_with_wrong_aggregation(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAXX', 10)

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MA', 10)

    def test_create_compaction_rule_without_dest_series(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)

    def test_create_compaction_rule_twice(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
                
    def test_create_compaction_rule_override_dest(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester2')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester2', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)

    def test_create_compaction_rule_from_target(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester2')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester_agg_max_10', 'tester2', 'AGGREGATION', 'MAX', 10)

    def test_create_compaction_rule_own(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.CREATERULE', 'tester', 'tester', 'AGGREGATION', 'MAX', 10)

    def test_create_compaction_rule_and_del_dest_series(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'AVG', 10)
            assert r.delete('tester_agg_max_10')

            start_ts = 1488823384L
            samples_count = 1500
            self._insert_data(r, 'tester', start_ts, samples_count, 5)

    def test_delete_rule(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('TS.CREATE', 'tester_agg_max_10')
            assert r.execute_command('TS.CREATE', 'tester_agg_min_20')
            assert r.execute_command('TS.CREATE', 'tester_agg_avg_30')
            assert r.execute_command('TS.CREATE', 'tester_agg_last_40')
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_max_10', 'AGGREGATION', 'MAX', 10)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_min_20', 'AGGREGATION', 'MIN', 20)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_avg_30', 'AGGREGATION', 'AVG', 30)
            assert r.execute_command('TS.CREATERULE', 'tester', 'tester_agg_last_40', 'AGGREGATION', 'LAST', 40)

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.DELETERULE', 'tester', 'non_existent')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.DELETERULE', 'non_existent', 'tester')

            assert len(self._get_ts_info(r, 'tester').rules) == 4
            assert r.execute_command('TS.DELETERULE', 'tester', 'tester_agg_avg_30')
            assert len(self._get_ts_info(r, 'tester').rules) == 3
            assert r.execute_command('TS.DELETERULE', 'tester', 'tester_agg_max_10')
            assert len(self._get_ts_info(r, 'tester').rules) == 2

    def test_empty_series(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            assert r.execute_command('DUMP', 'tester')

    def test_valid_labels(self):
        with self.redis() as r:
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

    def test_incrby(self):
        with self.redis() as r:
            r.execute_command('ts.create', 'tester')

            start_incr_time = int(time.time()*1000)
            for i in range(20):
                r.execute_command('ts.incrby', 'tester', '5')
                time.sleep(0.001)

            start_decr_time = int(time.time()*1000)
            for i in range(20):
                r.execute_command('ts.decrby', 'tester', '1.5')
                time.sleep(0.001)

            now = int(time.time()*1000)
            result = r.execute_command('TS.RANGE', 'tester', 0, now)
            assert result[-1][1] == '70'
            assert result[-1][0] <= now
            assert result[0][0] >= start_incr_time
            assert len(result) <= 40

    def test_incrby_with_timestamp(self):
        with self.redis() as r:
            r.execute_command('ts.create', 'tester')

            for i in range(20):
                assert r.execute_command('ts.incrby', 'tester', '5', 'TIMESTAMP', i) == i
            result = r.execute_command('TS.RANGE', 'tester', 0, 20)
            assert len(result) == 20
            assert result[19][1] == '100'

            query_res = r.execute_command('ts.incrby', 'tester', '5', 'TIMESTAMP', '*')/1000 
            cur_time = int(time.time())
            assert query_res >= cur_time
            assert query_res <= cur_time + 1

    def test_agg_min(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'min')

            expected_result = [[10, '123'], [20, '223'], [30, '323'], [40, '423']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_max(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'max')

            expected_result = [[10, '197'], [20, '297'], [30, '397'], [40, '497']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_avg(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'avg')

            expected_result = [[10, '156.5'], [20, '256.5'], [30, '356.5'], [40, '456.5']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_std_var_func(self):
        with self.redis() as r:
            raw_key = 'raw'
            std_key = 'std_key'
            var_key = 'var_key'

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
            r.execute_command('TS.ADD', raw_key, random_numbers, 0) #close time bucket
    
            assert abs(stdev - float(r.execute_command('TS.GET', std_key)[1])) < ALLOWED_ERROR
            assert abs(var - float(r.execute_command('TS.GET', var_key)[1])) < ALLOWED_ERROR

    def test_agg_std_p(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'std.p')

            expected_result = [[10, '25.869'], [20, '25.869'], [30, '25.869'], [40, '25.869']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            for i in range(len(expected_result)):
                assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR

    def test_agg_std_s(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'std.s')

            expected_result = [[10, '27.269'], [20, '27.269'], [30, '27.269'], [40, '27.269']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            for i in range(len(expected_result)):
                assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR

    def test_agg_var_p(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'var.p')

            expected_result = [[10, '669.25'], [20, '669.25'], [30, '669.25'], [40, '669.25']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            for i in range(len(expected_result)):
                assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR

    def test_agg_var_s(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'var.s')

            expected_result = [[10, '743.611'], [20, '743.611'], [30, '743.611'], [40, '743.611']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            for i in range(len(expected_result)):
                assert abs(float(expected_result[i][1]) - float(actual_result[i][1])) < ALLOWED_ERROR

    def test_agg_sum(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'sum')

            expected_result = [[10, '1565'], [20, '2565'], [30, '3565'], [40, '4565']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_count(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'count')

            expected_result = [[10, '10'], [20, '10'], [30, '10'], [40, '10']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_first(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'first')

            expected_result = [[10, '131'], [20, '231'], [30, '331'], [40, '431']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_last(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'last')

            expected_result = [[10, '184'], [20, '284'], [30, '384'], [40, '484']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

    def test_agg_range(self):
        with self.redis() as r:
            agg_key = self._insert_agg_data(r, 'tester', 'range')

            expected_result = [[10, '74'], [20, '74'], [30, '74'], [40, '74']]
            actual_result = r.execute_command('TS.RANGE', agg_key, 10, 50)
            assert expected_result == actual_result

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
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            rules = ['avg', 'sum', 'count', 'max', 'min']
            resolutions = [1, 3, 10, 1000]
            for rule in rules:
                for resolution in resolutions:
                    assert r.execute_command('TS.CREATE', 'tester_{}_{}'.format(rule, resolution))
                    assert r.execute_command('TS.CREATERULE', 'tester', 'tester_{}_{}'.format(rule, resolution),
                                             'AGGREGATION', rule, resolution)

            start_ts = 0
            samples_count = 501
            end_ts = start_ts + samples_count
            values = range(samples_count)
            self._insert_data(r, 'tester', start_ts, samples_count, values)
            r.execute_command('TS.ADD', 'tester', 3000, 7.77)
            
            for rule in rules:
                for resolution in resolutions:
                    actual_result = r.execute_command('TS.RANGE', 'tester_{}_{}'.format(rule, resolution),
                                                      start_ts, end_ts)
                    assert len(actual_result) == math.ceil(samples_count / float(resolution))
                    expected_result = self.calc_rule(rule, values, resolution)
                    assert self._get_series_value(actual_result) == expected_result
                    # last time stamp should be the beginning of the last bucket
                    assert self._get_ts_info(r, 'tester_{}_{}'.format(rule, resolution)).last_time_stamp == \
                                            (samples_count - 1) - (samples_count - 1) % resolution

            # test for results after empty buckets
            r.execute_command('TS.ADD', 'tester', 6000, 0)
            for rule in rules:
                for resolution in resolutions:
                    actual_result = r.execute_command('TS.RANGE', 'tester_{}_{}'.format(rule, resolution),
                                                      3000, 6000)
                    assert len(actual_result) == 1
                    assert self._get_series_value(actual_result) == [7.77] or \
                           self._get_series_value(actual_result) == [1]

    def test_automatic_timestamp(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester')
            curr_time = int(time.time()*1000)
            response_timestamp = r.execute_command('TS.ADD', 'tester', '*', 1)
            result = r.execute_command('TS.RANGE', 'tester', 0, int(time.time() * 1000))
            # test time difference is not more than 5 milliseconds
            assert result[0][0] - curr_time <= 5
            assert response_timestamp - curr_time <= 5

    def test_add_create_key(self):
        with self.redis() as r:
            ts = time.time()
            assert r.execute_command('TS.ADD', 'tester1', str(int(ts)), str(ts), 'RETENTION', '666', 'LABELS', 'name', 'blabla') == int(ts)
            info = self._get_ts_info(r, 'tester1')
            assert info.total_samples == 1L 
            assert info.retention_msecs == 666L
            assert info.labels == {'name': 'blabla'}

            assert r.execute_command('TS.ADD', 'tester2', str(int(ts)), str(ts), 'LABELS', 'name', 'blabla2', 'location', 'earth')
            info = self._get_ts_info(r, 'tester2')
            assert info.total_samples == 1L 
            assert info.labels == {'location': 'earth', 'name': 'blabla2'}

    def test_range_by_labels(self):
        start_ts = 1511885909L
        samples_count = 50

        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
            self._insert_data(r, 'tester1', start_ts, samples_count, 5)
            self._insert_data(r, 'tester2', start_ts, samples_count, 15)
            self._insert_data(r, 'tester3', start_ts, samples_count, 25)

            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
            assert [['tester1', [], expected_result]] == actual_result
            expected_result.reverse()
            actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
            assert [['tester1', [], expected_result]] == actual_result

            def build_expected(val, time_bucket):
                return [[long(i - i%time_bucket), str(val)] for i in range(start_ts, start_ts+samples_count+1, time_bucket)]
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'LAST', 5, 'FILTER', 'generation=x')
            expected_result = [['tester1', [], build_expected(5, 5)],
                    ['tester2', [], build_expected(15, 5)],
                    ['tester3', [], build_expected(25, 5)],
                    ]
            assert expected_result == actual_result
            assert expected_result[1:] == r.execute_command('TS.mrange', start_ts, start_ts + samples_count,
                                                            'AGGREGATION', 'LAST', 5, 'FILTER', 'generation=x', 'class!=middle')
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 3, 'AGGREGATION', 'LAST', 5, 'FILTER', 'generation=x')
            assert expected_result[0][2][:3] == actual_result[0][2]
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 5, 'FILTER', 'generation=x')
            assert [[1511885905L, '1']] == actual_result[0][2][:1]
            assert expected_result[0][2][1:9] == actual_result[0][2][1:9]
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 3, 'COUNT', 3, 'FILTER', 'generation=x')
            assert 3 == len(actual_result[0][2]) #just checking that agg count before count works
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 3, 'AGGREGATION', 'COUNT', 3, 'FILTER', 'generation=x')
            assert 3 == len(actual_result[0][2]) #just checking that agg count before count works
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'COUNT', 3, 'FILTER', 'generation=x')
            assert 18 == len(actual_result[0][2]) #just checking that agg count before count works

            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'invalid', 3, 'FILTER', 'generation=x')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'AGGREGATION', 'AVG', 'string', 'FILTER', 'generation=x')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'COUNT', 'string', 'FILTER', 'generation=x')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange - + FILTER') # missing args
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange - + RETLIF') # no filter word
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', 'string', start_ts + samples_count, 'FILTER', 'generation=x')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, 'string', 'FILTER', 'generation=x')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'generation+x')
    
            #issue 414
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=(bob,rudy,)')
            with pytest.raises(redis.ResponseError) as excinfo:
                assert r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'FILTER', 'name=(bob,,rudy)')

    def test_mrange_withlabels(self):
        start_ts = 1511885909L
        samples_count = 50

        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
            self._insert_data(r, 'tester1', start_ts, samples_count, 5)
            self._insert_data(r, 'tester2', start_ts, samples_count, 15)
            self._insert_data(r, 'tester3', start_ts, samples_count, 25)

            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            actual_result = r.execute_command('TS.mrange', start_ts, start_ts + samples_count, 'WITHLABELS', 'FILTER', 'name=bob')
            assert [['tester1', [['name', 'bob'], ['class', 'middle'], ['generation', 'x']], expected_result]] == actual_result         
            actual_result = r.execute_command('TS.mrange', start_ts + 1, start_ts + samples_count, 'WITHLABELS', 'AGGREGATION', 'COUNT', 1, 'FILTER', 'generation=x')
            # assert the labels length is 3 (name,class,generation) for each of the returned time-series
            assert len(actual_result[0][1]) == 3
            assert len(actual_result[1][1]) == 3
            assert len(actual_result[2][1]) == 3

    def test_range_count(self):
        start_ts = 1511885908L
        samples_count = 50

        with self.redis() as r:
            r.execute_command('TS.CREATE', 'tester1')
            for i in range(samples_count):
                r.execute_command('TS.ADD', 'tester1', start_ts + i, i)
            full_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
            assert len(full_results) == samples_count
            count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'COUNT', 10)
            assert count_results == full_results[:10]
            count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'COUNT', 10, 'AGGREGATION', 'COUNT', 3)
            assert len(count_results) == 10
            count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'AGGREGATION', 'COUNT', 3, 'COUNT', 10)
            assert len(count_results) == 10
            count_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'AGGREGATION', 'COUNT', 3)
            assert len(count_results) ==  math.ceil(samples_count / 3.0)
            
    def test_revrange(self):
        start_ts = 1511885908L
        samples_count = 200
        expected_results = []

        with self.redis() as r:
            r.execute_command('TS.CREATE', 'tester1', 'uncompressed')
            for i in range(samples_count):
                r.execute_command('TS.ADD', 'tester1', start_ts + i, i)
            actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev
            
            actual_results = r.execute_command('TS.RANGE', 'tester1', 1511885910L, 1511886000L)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 1511885910L, 1511886000L)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev

            actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev

            # with compression
            r.execute_command('DEL', 'tester1')
            r.execute_command('TS.CREATE', 'tester1')
            for i in range(samples_count):
                r.execute_command('TS.ADD', 'tester1', start_ts + i, i)
            actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev
            
            actual_results = r.execute_command('TS.RANGE', 'tester1', 1511885910L, 1511886000L)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 1511885910L, 1511886000L)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev

            actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
            actual_results_rev.reverse()
            assert actual_results == actual_results_rev

            actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'COUNT', 5)
            assert len(actual_results_rev) == 5
            assert actual_results_rev[0][0] > actual_results_rev[1][0]

    def test_mrevrange(self):
        start_ts = 1511885909L
        samples_count = 50
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
            self._insert_data(r, 'tester1', start_ts, samples_count, 5)
            self._insert_data(r, 'tester2', start_ts, samples_count, 15)
            self._insert_data(r, 'tester3', start_ts, samples_count, 25)        

            expected_result = [[start_ts+i, str(5)] for i in range(samples_count)]
            expected_result.reverse()
            actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
            assert [['tester1', [], expected_result]] == actual_result

            actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'COUNT', '5', 'FILTER', 'generation=x')
            assert actual_result == [['tester1',[],[[1511885958L, '5'], [1511885957L, '5'], [1511885956L, '5'], [1511885955L, '5'], [1511885954L, '5']]],
                                    ['tester2', [],[[1511885958L, '15'],[1511885957L, '15'],[1511885956L, '15'],[1511885955L, '15'],[1511885954L, '15']]],
                                    ['tester3', [],[[1511885958L, '25'],[1511885957L, '25'],[1511885956L, '25'],[1511885955L, '25'],[1511885954L, '25']]]]

            agg_result = r.execute_command('TS.mrange', 0, -1, 'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
            rev_agg_result = r.execute_command('TS.mrevrange', 0, -1, 'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
            rev_agg_result.reverse()
            assert rev_agg_result == agg_result

    def test_multilabel_filter(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x')
            
            assert r.execute_command('TS.ADD', 'tester1', 0, 1) == 0
            assert r.execute_command('TS.ADD', 'tester2', 0, 2) == 0
            assert r.execute_command('TS.ADD', 'tester3', 0, 3) == 0

            actual_result = r.execute_command('TS.mrange', 0, -1, 'WITHLABELS', 'FILTER', 'name=(bob,rudy)')
            assert actual_result[0][0] == 'tester1'
            assert actual_result[1][0] == 'tester2'

            actual_result = r.execute_command('TS.mrange', 0, -1, 'WITHLABELS', 'FILTER', 'name=(bob,rudy)', 'class!=(middle,top)')
            assert actual_result[0][0] == 'tester2'

            actual_result = r.execute_command('TS.mget', 'WITHLABELS', 'FILTER', 'name=(bob,rudy)')
            assert actual_result[0][0] == 'tester1'
            assert actual_result[1][0] == 'tester2'

            actual_result = r.execute_command('TS.mget', 'WITHLABELS', 'FILTER', 'name=(bob,rudy)', 'class!=(middle,top)')
            assert actual_result[0][0] == 'tester2'

    def test_label_index(self):
        with self.redis() as r:
            assert r.execute_command('TS.CREATE', 'tester1', 'LABELS', 'name', 'bob', 'class', 'middle', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester2', 'LABELS', 'name', 'rudy', 'class', 'junior', 'generation', 'x')
            assert r.execute_command('TS.CREATE', 'tester3', 'LABELS', 'name', 'fabi', 'class', 'top', 'generation', 'x', 'x', '2')
            assert r.execute_command('TS.CREATE', 'tester4', 'LABELS', 'name', 'anybody', 'class', 'top', 'type', 'noone', 'x', '2', 'z', '3')

            assert ['tester1', 'tester2', 'tester3'] == r.execute_command('TS.QUERYINDEX', 'generation=x')
            assert ['tester1', 'tester2'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'x=')
            assert ['tester3'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'x=2')
            assert ['tester3', 'tester4'] == r.execute_command('TS.QUERYINDEX', 'x=2')
            assert ['tester1', 'tester2'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class!=top')
            assert ['tester2'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class!=middle', 'x=')
            assert [] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=top', 'x=')
            assert ['tester3'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=top', 'z=')
            with pytest.raises(redis.ResponseError):
                r.execute_command('TS.QUERYINDEX', 'z=', 'x!=2')
            assert ['tester3'] == r.execute_command('TS.QUERYINDEX', 'z=', 'x=2')

            # Test filter list
            assert ['tester1', 'tester2'] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=(middle,junior)')
            assert [] == r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=(a,b,c)')
            assert r.execute_command('TS.QUERYINDEX', 'generation=x') == r.execute_command('TS.QUERYINDEX', 'generation=(x)')
            assert r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=()') == []
            assert r.execute_command('TS.QUERYINDEX', 'class=(middle,junior,top)', 'name!=(bob,rudy,fabi)') == ['tester4']
            with pytest.raises(redis.ResponseError):
                assert r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=(')
            with pytest.raises(redis.ResponseError):
                assert r.execute_command('TS.QUERYINDEX', 'generation=x', 'class=(ab')
            with pytest.raises(redis.ResponseError):
                assert r.execute_command('TS.QUERYINDEX', 'generation!=(x,y)')

    def test_series_ordering(self):
        with self.redis() as r:
            sample_len = 1024
            chunk_size = 4

            r.execute_command("ts.create", 'test_key', 0, chunk_size)
            for i in range(sample_len):
                r.execute_command("ts.add", 'test_key', i , i)

            res = r.execute_command('ts.range', 'test_key', 0, sample_len)
            i = 0
            for sample in res:
                assert sample == [i, str(i)]
                i += 1

    def test_madd(self):
        sample_len = 1024

        with self.redis() as r:
            r.execute_command("ts.create", 'test_key1')
            r.execute_command("ts.create", 'test_key2')
            r.execute_command("ts.create", 'test_key3')

            for i in range(sample_len):
                assert [i+1000L, i+3000L, i+6000L] == r.execute_command("ts.madd", 'test_key1', i+1000, i, 'test_key2', i+3000, i, 'test_key3', i+6000, i,)

            res = r.execute_command('ts.range', 'test_key1', 1000, 1000+sample_len)
            i = 0
            for sample in res:
                assert sample == [1000+i, str(i)]
                i += 1

            res = r.execute_command('ts.range', 'test_key2', 3000, 3000+sample_len)
            i = 0
            for sample in res:
                assert sample == [3000+i, str(i)]
                i += 1

            res = r.execute_command('ts.range', 'test_key3', 6000, 6000+sample_len)
            i = 0
            for sample in res:
                assert sample == [6000+i, str(i)]
                i += 1

    def test_partial_madd(self):
        with self.redis() as r:
            r.execute_command("ts.create", 'test_key1')
            r.execute_command("ts.create", 'test_key2')
            r.execute_command("ts.create", 'test_key3')

            now = int(time.time()*1000)
            res = r.execute_command("ts.madd", 'test_key1', "*", 10, 'test_key2', 2000, 20, 'test_key3', 3000, 30)
            assert now <= res[0] and now+2 >= res[0]
            assert 2000 == res[1]
            assert 3000 == res[2]

            res = r.execute_command("ts.madd", 'test_key1', now + 1000, 10, 'test_key2', 1000, 20, 'test_key3', 3001 , 30)
            assert (now + 1000, 3001) == (res[0], res[2])
            assert isinstance(res[1], redis.ResponseError)
            assert len(r.execute_command('ts.range', 'test_key1', "-", "+")) == 2
            assert len(r.execute_command('ts.range', 'test_key2', "-", "+")) == 1
            assert len(r.execute_command('ts.range', 'test_key3', "-", "+")) == 2
    
    def test_rule_timebucket_64bit(self):
        with self.redis() as r:
            BELOW_32BIT_LIMIT = 2147483647
            ABOVE_32BIT_LIMIT = 2147483648
            r.execute_command("ts.create", 'test_key', 'RETENTION', ABOVE_32BIT_LIMIT)
            r.execute_command("ts.create", 'below_32bit_limit')
            r.execute_command("ts.create", 'above_32bit_limit')
            r.execute_command("ts.createrule", 'test_key', 'below_32bit_limit', 'AGGREGATION', 'max', BELOW_32BIT_LIMIT)
            r.execute_command("ts.createrule", 'test_key', 'above_32bit_limit', 'AGGREGATION', 'max', ABOVE_32BIT_LIMIT)
            info = self._get_ts_info(r, 'test_key') 
            assert info.rules[0][1] == BELOW_32BIT_LIMIT
            assert info.rules[1][1] == ABOVE_32BIT_LIMIT

    def test_uncompressed(self):
        with self.redis() as r:
            # test simple commands
            r.execute_command('ts.create not_compressed UNCOMPRESSED')
            assert 1 == r.execute_command('ts.add not_compressed 1 3.5')
            assert 3.5 == float(r.execute_command('ts.get not_compressed')[1])
            assert 2 == r.execute_command('ts.add not_compressed 2 4.5')
            assert 3 == r.execute_command('ts.add not_compressed 3 5.5')
            assert 5.5 == float(r.execute_command('ts.get not_compressed')[1])
            assert [[1L, '3.5'], [2L, '4.5'], [3L, '5.5']] == \
                        r.execute_command('ts.range not_compressed 0 -1')
            info = self._get_ts_info(r, 'not_compressed')
            assert info.total_samples == 3 and info.memory_usage == 4128L

            # rdb load
            data = r.execute_command('dump', 'not_compressed')

        with self.redis() as r:
            r.execute_command('RESTORE', 'not_compressed', 0, data)
            assert [[1L, '3.5'], [2L, '4.5'], [3L, '5.5']] == \
                        r.execute_command('ts.range not_compressed 0 -1')
            info = self._get_ts_info(r, 'not_compressed')
            assert info.total_samples == 3 and info.memory_usage == 4128L
            # test deletion
            assert r.delete('not_compressed')

    
    def test_trim(self):
        with self.redis() as r:
            for mode in ["UNCOMPRESSED","COMPRESSED"]:
                samples = 2000
                chunk_size = 64
                remainder = samples % chunk_size
                total_chunk_count = math.ceil( float(samples) / float(chunk_size))
                r.execute_command('ts.create trim_me CHUNK_SIZE {0} RETENTION 10 {1}'.format(chunk_size,mode))
                r.execute_command('ts.create dont_trim_me CHUNK_SIZE {0} {1}'.format(chunk_size,mode))
                for i in range(samples):
                    r.execute_command('ts.add trim_me', i, i * 1.1)
                    r.execute_command('ts.add dont_trim_me', i, i * 1.1)
                
                trimmed_info = self._get_ts_info(r, 'trim_me')
                untrimmed_info = self._get_ts_info(r, 'dont_trim_me')
                assert 2 == trimmed_info.chunk_count
                assert samples == untrimmed_info.total_samples
                # extra test for uncompressed
                if mode == "UNCOMPRESSED":
                    assert chunk_size + remainder == trimmed_info.total_samples
                    assert total_chunk_count == untrimmed_info.chunk_count

                r.delete("trim_me")
                r.delete("dont_trim_me")


    def test_empty(self):
        with self.redis() as r:
            r.execute_command('ts.create empty')
            info = self._get_ts_info(r, 'empty')
            assert info.total_samples == 0
            assert [] == r.execute_command('TS.range empty 0 -1')
            assert [] == r.execute_command('TS.get empty')

            r.execute_command('ts.create empty_uncompressed uncompressed')
            info = self._get_ts_info(r, 'empty_uncompressed')
            assert info.total_samples == 0
            assert info.memory_usage == 8
            assert info.chunk_count == 0            
            assert [] == r.execute_command('TS.range empty_uncompressed 0 -1')
            assert [] == r.execute_command('TS.get empty')

    def test_gorilla(self):
        with self.redis() as r:
            r.execute_command('ts.create monkey')
            r.execute_command('ts.add monkey 0 1')
            r.execute_command('ts.add monkey 1 1')
            r.execute_command('ts.add monkey 2 1')
            r.execute_command('ts.add monkey 50 1')
            r.execute_command('ts.add monkey 51 1')
            r.execute_command('ts.add monkey 500 1')
            r.execute_command('ts.add monkey 501 1')
            r.execute_command('ts.add monkey 3000 1')
            r.execute_command('ts.add monkey 3001 1')
            r.execute_command('ts.add monkey 10000 1')
            r.execute_command('ts.add monkey 10001 1')
            r.execute_command('ts.add monkey 100000 1')
            r.execute_command('ts.add monkey 100001 1')
            r.execute_command('ts.add monkey 100002 1')
            r.execute_command('ts.add monkey 100004 1')
            r.execute_command('ts.add monkey 1000000 1')
            r.execute_command('ts.add monkey 1000001 1')
            r.execute_command('ts.add monkey 10000011000001 1')
            r.execute_command('ts.add monkey 10000011000002 1')
            expected_result = [[0L, '1'], [1L, '1'], [2L, '1'], [50L, '1'], [51L, '1'],
                               [500L, '1'], [501L, '1'], [3000L, '1'], [3001L, '1'], 
                               [10000L, '1'], [10001L, '1'], [100000L, '1'], [100001L, '1'],
                               [100002L, '1'], [100004L, '1'], [1000000L, '1'], [1000001L, '1'],
                               [10000011000001L, '1'], [10000011000002L, '1']]
            assert expected_result == r.execute_command('TS.range monkey 0 -1')

    def test_issue299(self):
        with self.redis() as r:
            r.execute_command('ts.create issue299')
            for i in range(1000):
                r.execute_command('ts.add issue299', i * 10, i)
            actual_result = r.execute_command('ts.range issue299 0 -1 aggregation avg 10')
            assert actual_result[0] == [0L, '0']
            actual_result = r.execute_command('ts.range issue299 0 -1 aggregation avg 100')
            assert actual_result[0] == [0L, '4.5']  

            r.execute_command('del issue299')
            r.execute_command('ts.create issue299')
            for i in range(100, 1000):
                r.execute_command('ts.add issue299', i * 10, i)
            actual_result = r.execute_command('ts.range issue299 0 -1 aggregation avg 10')
            assert actual_result[0] != [0L, '0']

    def test_issue358(self):
        filepath = "./issue358.txt"
        with self.redis() as r:
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

    def test_issue400(self):
        with self.redis() as r:
            times = 300
            r.execute_command('ts.create issue376 UNCOMPRESSED')
            for i in range(1, times):
                r.execute_command('ts.add issue376', i * 5, i)
            for i in range(1, times):
                range_res = r.execute_command('ts.range issue376', i * 5 - 1, i * 5 + 60)
                assert len(range_res) > 0
            for i in range(1, times):
                range_res = r.execute_command('ts.revrange issue376', i * 5 - 1, i * 5 + 60)
                assert len(range_res) > 0

    def test_dump_trimmed_series(self):
        with self.redis() as r:
            samples = 120
            start_ts = 1589461305983
            r.execute_command('ts.create test_key RETENTION 3000 CHUNK_SIZE 10 UNCOMPRESSED ')
            for i in range(1, samples):
                r.execute_command('ts.add test_key', start_ts + i * 1000, i)
            assert r.execute_command('ts.range test_key 0 -1') == \
                    [[1589461421983L, '116'], [1589461422983L, '117'], [1589461423983L, '118'], [1589461424983L, '119']]
            before = r.execute_command('ts.range test_key - +')
            dump = r.execute_command('dump test_key')
            r.execute_command('del test_key')
            r.execute_command('restore test_key 0', dump)
            assert r.execute_command('ts.range test_key - +') == before

class GlobalConfigTests(ModuleTestCase(REDISTIMESERIES, 
        module_args=['COMPACTION_POLICY', 'max:1m:1d;min:10s:1h;avg:2h:10d;avg:3d:100d'])):
    def test_autocreate(self):
        with self.redis() as r:
            assert r.execute_command('TS.ADD tester 1980 0 LABELS name',
                                     'brown color pink') == 1980
            keys = r.execute_command('keys *')
            keys = sorted(keys)
            assert keys == ['tester', 'tester_AVG_259200000', 'tester_AVG_7200000', 'tester_MAX_1', 'tester_MIN_10000']
            r.execute_command('TS.ADD tester 1981 1')

            r.execute_command('set exist_MAX_1 foo')
            r.execute_command('TS.ADD exist 1980 0')
            keys = r.execute_command('keys *')
            keys = sorted(keys)
            assert keys == ['exist', 'exist_AVG_259200000', 'exist_AVG_7200000', 'exist_MAX_1', 'exist_MIN_10000',
                            'tester', 'tester_AVG_259200000', 'tester_AVG_7200000', 'tester_MAX_1', 'tester_MIN_10000']
            r.execute_command('TS.ADD exist 1981 0')

########## Test init args ##########
def ModuleArgsTestCase(good, args):
    class _Class(ModuleTestCase(REDISTIMESERIES, module_args=args)):
        def test(self):
            ping = False
            try:
                ping = self.cmd('PING')
            except Exception:
                delattr(self, '_server') # server died, do not try to kill it
            assert good and ping or not good and not ping
    return _Class

class RedisTimeseriesInitTestRetSuccessNew(ModuleArgsTestCase(True, ['RETENTION_POLICY', '100'])):
    pass

class RedisTimeseriesInitTestRetFailStrNew(ModuleArgsTestCase(False, ['RETENTION_POLICY', 'RTS'])):
    pass

class RedisTimeseriesInitTestRetFailNeg(ModuleArgsTestCase(False, ['RETENTION_POLICY', -1])):
    pass

class RedisTimeseriesInitTestMaxSamplesSuccess(ModuleArgsTestCase(True, ['MAX_SAMPLE_PER_CHUNK', '100'])):
    pass

class RedisTimeseriesInitTestMaxSamplesFailStr(ModuleArgsTestCase(False, ['MAX_SAMPLE_PER_CHUNK', 'RTS'])):
    pass

class RedisTimeseriesInitTestMaxSamplesFailNeg(ModuleArgsTestCase(False, ['MAX_SAMPLE_PER_CHUNK', -1])):
    pass

class RedisTimeseriesInitTestPolicySuccess(ModuleArgsTestCase(True, ['COMPACTION_POLICY', 'max:1m:1d;min:10s:1h;avg:2h:10d;avg:3d:100d'])):
    pass

class RedisTimeseriesInitTestPolicyFail(ModuleArgsTestCase(False, ['COMPACTION_POLICY', 'RTS'])):
    pass