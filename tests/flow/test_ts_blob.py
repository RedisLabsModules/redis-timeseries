import math
import time

import sys

import pytest
import redis
from RLTest import Env
from test_helper_classes import SAMPLE_SIZE, _get_ts_info



def test_blob(self):

    with Env().getClusterConnectionIfNeeded() as r:

        r.execute_command('ts.create', 'blob1', 'BLOB')

        assert b'blob' == _get_ts_info(r, 'blob1').type
        r.execute_command('ts.add', 'blob2', 1, 'value1', 'BLOB')
        assert b'blob' == _get_ts_info(r, 'blob1').type
        res = r.execute_command('ts.range', 'blob2', '-', '+')

        assert len(res) == 1
        assert res[0][0] == 1
        assert res[0][1] == b'value1'
        r.execute_command('ts.add', 'blob2', 2, 'value2' )
        r.execute_command('ts.add', 'blob2', 3, 'value3' )
        res = r.execute_command('ts.range', 'blob2', '-', '+')

        assert len(res) == 3
        ### upsert ###
        r.execute_command('ts.add', 'blob2', 2, 'new_value2')
        res = r.execute_command('ts.range', 'blob2', '-', '+')

        assert len(res) == 3

        assert res[0][0] == 1
        assert res[0][1] == b'value1'
        assert res[1][0] == 2
        assert res[1][1] == b'new_value2'
        assert res[2][0] == 3
        assert res[2][1] == b'value3'

        ### aggregation count ###
        res = r.execute_command('ts.range', 'blob2', '-', '+', 'AGGREGATION', 'count', 10)

        assert res[0][1] == b'3'

        ### Forbidden commands ###
        forbidden = False;

        try:
            res = r.execute_command('ts.incrby', 'blob2', 1)
        except:
            forbidden = True;

        assert forbidden == True

        forbidden = False;

        try:
	        res = r.execute_command('ts.decrby', 'blob2', 1)
        except:
            forbidden = True;

        assert forbidden == True

        key_name='blob3{abc}'

        first_agg_key_name = '{}_first'.format(key_name)
        last_agg_key_name = '{}_last'.format(key_name)
        count_agg_key_name = '{}_count'.format(key_name)

        ### downsampling ###
        res = r.execute_command('ts.create', key_name, 'BLOB')
        res = r.execute_command('ts.create', first_agg_key_name, 'BLOB')
        res = r.execute_command('ts.create', last_agg_key_name, 'BLOB')
        res = r.execute_command('ts.create', count_agg_key_name, 'BLOB')

        assert r.execute_command('ts.createrule', key_name, first_agg_key_name, 'AGGREGATION', 'first', 3)
        assert r.execute_command('ts.createrule', key_name, last_agg_key_name, 'AGGREGATION', 'last', 3)

        forbidden = False;
        try:
            res = r.execute_command('ts.createrule', key_name, count_agg_key_name, 'AGGREGATION', 'count', 3)
        except:
            # must catch 'TSDB: the destination key is of binary type and cannot hold an aggregation count'
            forbidden = True;

        assert forbidden == True

        # re-create it with scalar type
        r.execute_command('del', count_agg_key_name)

        res = r.execute_command('ts.create', count_agg_key_name)

        res = r.execute_command('ts.createrule', key_name, count_agg_key_name, 'AGGREGATION', 'count', 3)
        # test save of empty aggregated count

        for i in range(1,10):
            r.execute_command('ts.add', key_name, i, 'value'+str(i) )

        res = r.execute_command('ts.range', last_agg_key_name, '-', '+')

        assert res[0][0] == 0
        assert res[0][1] == b'value2'

        assert res[1][0] == 3
        assert res[1][1] == b'value5'

        assert res[2][0] == 6
        assert res[2][1] == b'value8'

        res = r.execute_command('ts.range', first_agg_key_name, '-', '+')

        assert res[0][0] == 0
        assert res[0][1] == b'value1'

        assert res[1][0] == 3
        assert res[1][1] == b'value3'

        assert res[2][0] == 6
        assert res[2][1] == b'value6'

        res = r.execute_command('ts.range', count_agg_key_name, '-', '+')
        assert len(res) == 3

        # test save of empty blob
        res = r.execute_command('ts.create', 'empty', 'BLOB')

        # When server is started with slaves, a
        # "Background save already in progress" can happen.

        while True:
            try:
                res = r.execute_command('SAVE')
            except Exception as err:
                print(str(err), " -> retrying")
                time.sleep(1);
            else:
                break;

        r.execute_command('DUMP', key_name)
        r.execute_command('del', key_name)

        res = r.execute_command('ts.range', last_agg_key_name, '-', '+')
        ### Test after deletion of source (blob must be unchanged) ###

        assert res[0][1] == b'value2'
        assert res[1][1] == b'value5'
        assert res[2][1] == b'value8'
