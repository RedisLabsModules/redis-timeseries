from RLTest import Env


def test_revrange():
    start_ts = 1511885908
    samples_count = 200
    expected_results = []

    with Env().getConnection() as r:
        r.execute_command('TS.CREATE', 'tester1', 'uncompressed')
        for i in range(samples_count):
            r.execute_command('TS.ADD', 'tester1', start_ts + i, i)
        actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1)
        actual_results_rev.reverse()
        assert actual_results == actual_results_rev

        actual_results = r.execute_command('TS.RANGE', 'tester1', 1511885910, 1511886000)
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 1511885910, 1511886000)
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

        actual_results = r.execute_command('TS.RANGE', 'tester1', 1511885910, 1511886000)
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 1511885910, 1511886000)
        actual_results_rev.reverse()
        assert actual_results == actual_results_rev

        actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'AGGREGATION', 'sum', 50)
        actual_results_rev.reverse()
        assert actual_results == actual_results_rev

        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'COUNT', 5)
        actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1)
        actual_results.reverse()
        assert len(actual_results_rev) == 5
        assert actual_results[0:5] == actual_results_rev[0:5]

        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', 0, -1, 'OFFSET', 2)
        actual_results = r.execute_command('TS.RANGE', 'tester1', 0, -1, 'OFFSET', 2)
        actual_results.reverse()
        assert actual_results == actual_results_rev

        # Set the offset to (start+1) % 50, so that it's correct during the aggregation process
        actual_results = r.execute_command('TS.RANGE', 'tester1', start_ts + 1, -1, 'OFFSET', (start_ts + 1) % 50,
                                           'AGGREGATION', 'sum', 50)
        # The revrange offset will be the 'inverse' of the range offset, ie: 50 - range_offset
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', start_ts + 1, -1, 'OFFSET', (start_ts + 1) % 50,
                                               'AGGREGATION', 'sum', 50)
        actual_results.reverse()
        assert actual_results == actual_results_rev

        actual_results = r.execute_command('TS.RANGE', 'tester1', start_ts + 1, start_ts+41, 'OFFSET', (start_ts + 1) % 5,
                                           'AGGREGATION', 'sum', 5)
        actual_results_rev = r.execute_command('TS.REVRANGE', 'tester1', start_ts + 1, start_ts+41, 'OFFSET', (start_ts + 1) % 5,
                                               'AGGREGATION', 'sum', 5)
        actual_results.reverse()
        assert actual_results == actual_results_rev


def test_issue400():
    with Env().getConnection() as r:
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
