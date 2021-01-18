from RLTest import Env
from test_helper_classes import _insert_data


def test_mrevrange():
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
        expected_result.reverse()
        actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'FILTER', 'name=bob')
        assert [[b'tester1', [], expected_result]] == actual_result

        actual_result = r.execute_command('TS.mrevrange', start_ts, start_ts + samples_count, 'COUNT', '5', 'FILTER',
                                          'generation=x')
        assert actual_result == [[b'tester1', [],
                                  [[1511885958, b'5'], [1511885957, b'5'], [1511885956, b'5'], [1511885955, b'5'],
                                   [1511885954, b'5']]],
                                 [b'tester2', [],
                                  [[1511885958, b'15'], [1511885957, b'15'], [1511885956, b'15'], [1511885955, b'15'],
                                   [1511885954, b'15']]],
                                 [b'tester3', [],
                                  [[1511885958, b'25'], [1511885957, b'25'], [1511885956, b'25'], [1511885955, b'25'],
                                   [1511885954, b'25']]]]

        agg_result = r.execute_command('TS.mrange', 0, -1, 'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
        rev_agg_result = r.execute_command('TS.mrevrange', 0, -1, 'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
        rev_agg_result.reverse()
        assert rev_agg_result == agg_result
        last_results = list(agg_result)
        last_results.reverse()
        last_results = last_results[0:3]
        assert r.execute_command('TS.mrevrange', 0, -1, 'AGGREGATION', 'sum', 50, 'COUNT', 3, 'FILTER', 'name=bob')[0][
                   2] == last_results

        agg_result = r.execute_command('TS.mrange', start_ts + 1, -1, 'OFFSET', (start_ts + 1) % 50,
                                       'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
        rev_agg_result = r.execute_command('TS.mrevrange', start_ts + 1, -1, 'OFFSET', (start_ts + 1) % 50,
                                       'AGGREGATION', 'sum', 50, 'FILTER', 'name=bob')[0][2]
        rev_agg_result.reverse()
        assert rev_agg_result == agg_result
