from collections import defaultdict

from utils import Env
import pytest
import redis
import create_test_rdb_file

def test_groupby_reduce_errors():
    env = Env()
    with env.getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.ADD', 's1', 1, 100, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's2', 2, 55, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's3', 2, 40, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'system')
        assert r.execute_command('TS.ADD', 's1', 2, 95)

        # test wrong arity
        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY')

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name')

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'abc', 'abc')

        with pytest.raises(redis.ResponseError) as excinfo:
            assert r.execute_command('TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'REDUCE', 'bla')

def test_groupby_reduce():
    env = Env()
    with env.getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.ADD', 's1', 1, 100, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's2', 2, 55, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's3', 2, 40, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'system')
        assert r.execute_command('TS.ADD', 's1', 2, 95)

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'REDUCE', 'max')
        serie1 = actual_result[0]
        serie1_name = serie1[0]
        serie1_labels = serie1[1]
        serie1_values = serie1[2]
        env.assertEqual(serie1_values, [[2, b'40']])
        env.assertEqual(serie1_name, b'metric_name=system')
        env.assertEqual(serie1_labels[0][0], b'metric_name')
        env.assertEqual(serie1_labels[0][1], b'system')
        serie2 = actual_result[1]
        serie2_name = serie2[0]
        serie2_labels = serie2[1]
        serie2_values = serie2[2]
        env.assertEqual(serie2_name, b'metric_name=user')
        env.assertEqual(serie2_labels[0][0], b'metric_name')
        env.assertEqual(serie2_labels[0][1], b'user')
        env.assertEqual(serie2_labels[1][0], b'__reducer__')
        env.assertEqual(serie2_labels[1][1], b'max')
        env.assertEqual(serie2_labels[2][0], b'__source__')
        env.assertEqual(sorted(serie2_labels[2][1].decode("ascii").split(",")), ['s1', 's2'])
        env.assertEqual(serie2_values, [[1, b'100'], [2, b'95']])

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'REDUCE', 'sum')
        serie2 = actual_result[1]
        serie2_values = serie2[2]
        env.assertEqual(serie2_values, [[1, b'100'], [2, b'150']])

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'REDUCE', 'min')
        serie2 = actual_result[1]
        serie2_values = serie2[2]
        env.assertEqual(serie2_values, [[1, b'100'], [2, b'55']])

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'COUNT', 1, 'FILTER', 'metric_family=cpu', 'GROUPBY', 'metric_name', 'REDUCE', 'min')
        serie2 = actual_result[1]
        serie2_values = serie2[2]
        env.assertEqual(serie2_values, [[1, b'100']])

def test_groupby_reduce_empty():
    env = Env()
    with env.getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.ADD', 's1', 1, 100, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's2', 2, 55, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'user')
        assert r.execute_command('TS.ADD', 's3', 2, 40, 'LABELS', 'metric_family', 'cpu', 'metric_name', 'system')
        assert r.execute_command('TS.ADD', 's1', 2, 95)

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'metric_family=cpu', 'GROUPBY', 'labelX', 'REDUCE', 'max')
        env.assertEqual(actual_result, [])

def test_groupby_reduce_multiple_groups():
    env = Env()
    with env.getClusterConnectionIfNeeded() as r:
        assert r.execute_command('TS.ADD', 's1', 1, 100, 'LABELS', 'HOST', 'A', 'REGION', 'EU', 'PROVIDER', 'AWS')
        assert r.execute_command('TS.ADD', 's2', 1, 55, 'LABELS', 'HOST', 'B', 'REGION', 'EU', 'PROVIDER', 'AWS')
        assert r.execute_command('TS.ADD', 's2', 2, 90)
        assert r.execute_command('TS.ADD', 's3', 2, 40, 'LABELS', 'HOST', 'C', 'REGION', 'US', 'PROVIDER', 'AWS')

        actual_result = r.execute_command(
            'TS.mrange', '-', '+', 'WITHLABELS', 'FILTER', 'PROVIDER=AWS', 'GROUPBY', 'REGION', 'REDUCE', 'max')
        serie1 = actual_result[0]
        serie1_name = serie1[0]
        serie1_labels = serie1[1]
        serie1_values = serie1[2]
        env.assertEqual(serie1_values, [[1, b'100'],[2, b'90']])
        env.assertEqual(serie1_name, b'REGION=EU')
        env.assertEqual(serie1_labels[0][0], b'REGION')
        env.assertEqual(serie1_labels[0][1], b'EU')
        env.assertEqual(serie1_labels[1][0], b'__reducer__')
        env.assertEqual(serie1_labels[1][1], b'max')
        env.assertEqual(serie1_labels[2][0], b'__source__')
        env.assertEqual(sorted(serie1_labels[2][1].decode("ascii").split(",")), ['s1', 's2'])
        serie2 = actual_result[1]
        serie2_name = serie2[0]
        serie2_labels = serie2[1]
        serie2_values = serie2[2]
        env.assertEqual(serie2_values, [[2, b'40']])
        env.assertEqual(serie2_name, b'REGION=US')
        env.assertEqual(serie2_labels[0][0], b'REGION')
        env.assertEqual(serie2_labels[0][1], b'US')
        env.assertEqual(serie2_labels[1][0], b'__reducer__')
        env.assertEqual(serie2_labels[1][1], b'max')
        env.assertEqual(serie2_labels[2][0], b'__source__')
        env.assertEqual(serie2_labels[2][1], b's3')

def truncate_month(date):
    return "-".join(date.split("-")[0:2])

def test_filterby():
    env = Env()
    high_temps = defaultdict(lambda : defaultdict(lambda: 0))
    specific_days = defaultdict(lambda : defaultdict(lambda: 0))
    days = [1335830400000, 1338508800000]
    for row in create_test_rdb_file.read_from_disk():
        timestamp = create_test_rdb_file.parse_timestamp(row[0])
        country = row[create_test_rdb_file.Country].replace('(', '[').replace(')', ']')
        if timestamp in days:
            specific_days[country][timestamp] += 1

        if row[1] and float(row[1]) >= 30:
            if timestamp > 0:
                high_temps[country][timestamp] += 1

    with env.getClusterConnectionIfNeeded() as r:
        create_test_rdb_file.load_into_redis(r)

        def assert_results(results, expected_results):
            for row in results:
                country = row[1][0][1].decode()
                points = dict([(point[0], int(point[1])) for point in row[2]])
                for k in points:
                    env.assertEqual(points[k], expected_results[country][k], message="timestamp {} not equal".format(k))
                env.assertEqual(points, expected_results[country], message="country {} not eq".format(country))

        results = r.execute_command("TS.MRANGE", "-", "+",
                          "withlabels", "FILTER_BY_VALUE", 30, 100,
                          "AGGREGATION", "count", 3600000,
                          "filter", "metric=temperature", "groupby", "country", "reduce", "sum")
        assert_results(results, high_temps)

        results = r.execute_command("TS.MRANGE", "-", "+",
                                    "withlabels", "FILTER_BY_TS", 1335830400000, 1338508800000,
                                    "AGGREGATION", "count", 3600000,
                                    "filter", "metric=temperature", "groupby", "country", "reduce", "sum")
        assert_results(results, specific_days)
