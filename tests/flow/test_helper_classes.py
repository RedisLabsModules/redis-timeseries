import builtins
import math

ALLOWED_ERROR = 0.001
SAMPLE_SIZE = 16


def _fill_data(r, key):
    start_ts = 1600001540000
    date_ranges = [(start_ts, start_ts + 100), (start_ts + 1000, start_ts + 1100)]
    for start, end in date_ranges:
        for ts in range(start, end):
            r.execute_command('TS.ADD', key, ts, ts)
    return date_ranges


def _get_series_value(ts_key_result):
    """
    Get only the values from the time stamp series
    :param ts_key_result: the output of ts.range command (pairs of timestamp and value)
    :return: float values of all the values in the series
    """
    return [float(value[1]) for value in ts_key_result]


def _assert_alter_cmd(r, key, start_ts, end_ts,
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

    actual_result = _get_ts_info(r, key)

    if expected_data:
        actual_data = r.execute_command('TS.range', key, start_ts, end_ts)
        assert expected_data == actual_data
    if expected_retention:
        assert expected_retention == actual_result.retention_msecs
    if expected_labels:
        assert list_to_dict(expected_labels) == actual_result.labels
    if expected_chunk_size:
        assert expected_chunk_size == actual_result.chunk_size_bytes


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


def calc_rule(rule, values, bucket_size):
    """
    Calculate the downsampling with the given rule
    :param rule: 'avg' / 'max' / 'min' / 'sum' / 'count'
    :param values: original series values
    :param bucket_size: bucket size for downsampling
    :return: the values of the series after downsampling
    """
    if rule == 'avg':
        return _calc_downsampling_series(values, bucket_size, lambda x: float(sum(x)) / len(x))
    elif rule in ['sum', 'max', 'min']:
        return _calc_downsampling_series(values, bucket_size, getattr(builtins, rule))
    elif rule == 'count':
        return _calc_downsampling_series(values, bucket_size, len)


def _insert_agg_data(redis, key, agg_type, chunk_type="", fromTS=10, toTS=50, key_create_args=None):
    agg_key = '%s_agg_%s_10' % (key, agg_type)

    if key_create_args is None:
        key_create_args = []

    assert redis.execute_command('TS.CREATE', key, chunk_type, *key_create_args)
    assert redis.execute_command('TS.CREATE', agg_key, chunk_type, *key_create_args)
    assert redis.execute_command('TS.CREATERULE', key, agg_key, "AGGREGATION", agg_type, 10)

    values = (31, 41, 59, 26, 53, 58, 97, 93, 23, 84)
    for i in range(fromTS, toTS):
        assert redis.execute_command('TS.ADD', key, i, i // 10 * 100 + values[i % 10])
    # close last bucket
    assert redis.execute_command('TS.ADD', key, toTS + 1000, 0)

    return agg_key


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
        if type(value) == list:
            value_to_insert = value[i]
        else:
            value_to_insert = float(value)
        actual_result = redis.execute_command('TS.ADD', key, start_ts + i, value_to_insert)
        if type(actual_result) == int:
            assert actual_result == int(start_ts + i)
        else:
            assert actual_result


def list_to_dict(aList):
    return {aList[i][0]: aList[i][1] for i in range(len(aList))}


def _get_ts_info(redis, key):
    return TSInfo(redis.execute_command('TS.INFO', key))


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
    chunk_size_bytes = None
    chunk_type = None
    chunks = None
    type = None

    def __str__(self):
        return "rules:" + str(self.rules) + \
               ",labels:"+str(self.labels) +\
               ",sourceKey:"+str(self.sourceKey)+\
               ",chunk_count:"+str(self.chunk_count)+\
               ",memory_usage:"+str(self.memory_usage)+\
               ",total_samples:"+str(self.total_samples)+\
               ",retention_msecs:"+str(self.retention_msecs)+\
               ",last_time_stamp:"+str(self.last_time_stamp)+\
               ",first_time_stamp:"+str(self.first_time_stamp)+\
               ",chunk_size_bytes:"+str(self.chunk_size_bytes)+\
               ",chunk_type:"+str(self.chunk_type)+\
               ",Chunks:"+str(self.chunks)+\
               ",type:"+str(self.type)

    def __init__(self, args):
        response = dict(zip(args[::2], args[1::2]))
        if b'rules' in response: self.rules = response[b'rules']
        if b'sourceKey' in response: self.sourceKey = response[b'sourceKey']
        if b'chunkCount' in response: self.chunk_count = response[b'chunkCount']
        if b'labels' in response: self.labels = list_to_dict(response[b'labels'])
        if b'memoryUsage' in response: self.memory_usage = response[b'memoryUsage']
        if b'totalSamples' in response: self.total_samples = response[b'totalSamples']
        if b'retentionTime' in response: self.retention_msecs = response[b'retentionTime']
        if b'lastTimestamp' in response: self.last_time_stamp = response[b'lastTimestamp']
        if b'firstTimestamp' in response: self.first_time_stamp = response[b'firstTimestamp']
        if b'chunkSize' in response: self.chunk_size_bytes = response[b'chunkSize']
        if b'chunkType' in response: self.chunk_type = response[b'chunkType']
        if b'Chunks' in response: self.chunks = response[b'Chunks']
        if b'type' in response: self.type = response[b'type']

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
               self.chunk_size_bytes == other.chunk_size_bytes and \
               self.chunks == other.chunks and \
               self.type == other.type
