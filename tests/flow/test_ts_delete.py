from RLTest import Env

CHUNK_TYPES = ["COMPRESSED:TURBO_GORILLA","","COMPRESSED","COMPRESSED:GORILLA","UNCOMPRESSED"]

def test_ts_del_uncompressed():
    # total samples = 101
    sample_len = 101
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command("ts.create", 'test_key', 'uncompressed')

        for i in range(sample_len):
            assert i == r.execute_command("ts.add", 'test_key', i, '1')

        res = r.execute_command('ts.range', 'test_key', 0, 100)
        i = 0
        for sample in res:
            assert sample == [i, '1'.encode('ascii')]
            i += 1
        r.execute_command('ts.del', 'test_key', 0, 100)
        res = r.execute_command('ts.range', 'test_key', 0, 100)
        assert len(res) == 0


def test_ts_del_uncompressed_in_range():
    sample_len = 101
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command("ts.create", 'test_key', 'uncompressed')

        for i in range(sample_len):
            assert i == r.execute_command("ts.add", 'test_key', i, '1')

        res = r.execute_command('ts.range', 'test_key', 0, 100)
        i = 0
        for sample in res:
            assert sample == [i, '1'.encode('ascii')]
            i += 1
        # delete 11 samples
        r.execute_command('ts.del', 'test_key', 50, 60)
        res = r.execute_command('ts.range', 'test_key', 0, 100)
        assert len(res) == 90


def test_ts_del_compressed():
    sample_len = 101
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command("ts.create", 'test_key')

        for i in range(sample_len):
            assert i == r.execute_command("ts.add", 'test_key', i, '1')

        res = r.execute_command('ts.range', 'test_key', 0, 100)
        i = 0
        for sample in res:
            assert sample == [i, '1'.encode('ascii')]
            i += 1
        r.execute_command('ts.del', 'test_key', 0, 100)
        res = r.execute_command('ts.range', 'test_key', 0, 100)
        assert len(res) == 0


def test_ts_del_compressed_multi_chunk():
    sample_len = 1001
    with Env().getClusterConnectionIfNeeded() as r:
        r.execute_command("ts.create", 'test_key')

        for i in range(sample_len):
            assert i == r.execute_command("ts.add", 'test_key', i, '1')

        res = r.execute_command('ts.range', 'test_key', 0, sample_len - 1)
        i = 0
        for sample in res:
            assert sample == [i, '1'.encode('ascii')]
            i += 1
        r.execute_command('ts.del', 'test_key', 0, 999)
        res = r.execute_command('ts.range', 'test_key', 0, sample_len - 1)
        assert len(res) == 1


def test_ts_del_out_range():
    sample_len = 10000
    for CHUNK_TYPE in CHUNK_TYPES:
        e = Env()
        e.flush()
        with e.getClusterConnectionIfNeeded() as r:
            r.execute_command("ts.create", 'test_key', CHUNK_TYPE)

            for i in range(sample_len):
                assert i + 100 == r.execute_command("ts.add", 'test_key', i + 100, '1')

            res = r.execute_command('ts.range', 'test_key', 0 + 100, sample_len + 100 - 1)
            i = 0
            for sample in res:
                assert sample == [i + 100, '1'.encode('ascii')]
                i += 1
            r.execute_command('ts.del', 'test_key', 0, 10000+100)
            res = r.execute_command('ts.range', 'test_key', 0 + 100, sample_len + 100 - 1)
            assert len(res) == 0
