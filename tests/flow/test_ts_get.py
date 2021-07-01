import pytest
import redis
import time 
import _thread
from RLTest import Env

def test_get_timestamp(self):
    with Env().getConnection() as r:
        
        self.assertTrue(r.execute_command("TS.CREATE", "X"))
        self.assertEqual(r.execute_command("TS.GET", "X"), [])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "0"), [])
        
        self.assertTrue(r.execute_command("TS.ADD", "X" ,"2" ,"1.2"))
        self.assertEqual(r.execute_command("TS.GET", "X"), [2, b'1.2'])
        self.assertEqual(r.execute_command("TS.GET", "X"), [2 ,b'1.2'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "0"), [2, b'1.2'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "1"), [2, b'1.2'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "2"), [])
        
        self.assertTrue(r.execute_command("TS.ADD", "X" ,"3" ,"2.1"))
        self.assertEqual(r.execute_command("TS.GET", "X"), [3, b'2.1'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "1"), [2, b'1.2'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "2"), [3, b'2.1'])
        self.assertEqual(r.execute_command("TS.GET", "X", "TIMESTAMP", "3"), [])
        
def test_bad_timestamp(self):
    with Env().getConnection() as r:
        
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command("TS.GET", "bad_x", "TIMESTAMP", "0")        
        
        self.assertTrue(r.execute_command("TS.CREATE", "bad_x"))
        self.assertTrue(r.execute_command("TS.ADD", "bad_x" ,"6" ,"4.5"))
        self.assertEqual(r.execute_command("TS.GET", "bad_x", "TIMESTAMP", "4"), [6, b'4.5'])
        
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command("TS.GET", "bad_x", "TIMESTAMP", "12d")        

        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command("TS.GET", "bad_x", "TIMESTAMP", "-5")        
        
def async_add(r, key, event_time, event_value, block=1):
    time.sleep(block)
    r.execute_command("TS.ADD", key, event_time, event_value)

def test_block(self):
    with Env().getConnection() as r:        
        r.execute_command("TS.CREATE", "blocked_key")        
        self.assertEqual([], r.execute_command("TS.GET", "blocked_key", "BLOCK", "1"))        
       
        # test blocked forever 
        _thread.start_new_thread( async_add, (r, "blocked_key", 100, 22, 0.2) )
        self.assertEqual([100, b'22'], r.execute_command("TS.GET", "blocked_key", "BLOCK", "0"))

        _thread.start_new_thread( async_add, (r, "blocked_key", 102, 31, 0.1) )
        self.assertEqual([102, b'31'], r.execute_command("TS.GET", "blocked_key", "BLOCK", "200"))
        
        # test block timed out
        _thread.start_new_thread( async_add, (r, "blocked_key", 103, 32, 0.2) )
        self.assertEqual([], r.execute_command("TS.GET", "blocked_key", "BLOCK", "100"))

        # wrong block time
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command("TS.GET", "blocked_key", "BLOCK", "-1")
        with pytest.raises(redis.ResponseError) as excinfo:
            r.execute_command("TS.GET", "blocked_key", "BLOCK", "32d")


def test_block_timestamp(self):
    with Env().getConnection() as r:        
        r.execute_command("TS.CREATE", "blocked_ts_key")        
        r.execute_command("TS.ADD", "blocked_ts_key", 201, 22.2)
        r.execute_command("TS.ADD", "blocked_ts_key", 202, 39.1)
        r.execute_command("TS.ADD", "blocked_ts_key", 203, -78.1)
       
        self.assertEqual([201, b'22.2'], r.execute_command("TS.GET", "blocked_ts_key", "TIMESTAMP", 200, "BLOCK", "200"))
        self.assertEqual([202, b'39.1'], r.execute_command("TS.GET", "blocked_ts_key", "TIMESTAMP", 201, "BLOCK", "200"))
        self.assertEqual([203, b'-78.1'], r.execute_command("TS.GET", "blocked_ts_key", "TIMESTAMP", 202, "BLOCK", "200"))

        _thread.start_new_thread( async_add, (r, "blocked_ts_key", 204, 33.5, 0.1) )
        self.assertEqual([204, b'33.5'], r.execute_command("TS.GET", "blocked_ts_key", "TIMESTAMP", 203, "BLOCK", "200"))