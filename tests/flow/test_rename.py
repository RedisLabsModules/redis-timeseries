from test_helper_classes import TSInfo

def test_rename_src(env):
    with env.getClusterConnectionIfNeeded() as r:

        assert r.execute_command('TS.CREATE', 'a1{1}')
        assert r.execute_command('TS.CREATE', 'b{1}')

        env.assertTrue(r.execute_command('RENAME', 'a1{1}', 'a2{1}'))
        aInfo = TSInfo(r.execute_command('TS.INFO', 'a2{1}'))
        env.assertEqual(aInfo.sourceKey, None)
        env.assertEqual(aInfo.rules, [])
        
        assert r.execute_command('TS.CREATERULE', 'a2{1}', 'b{1}', 'AGGREGATION', 'AVG', 5000)
        bInfo = TSInfo(r.execute_command('TS.INFO', 'b{1}'))
        env.assertEqual(bInfo.sourceKey, b'a2{1}')
        env.assertEqual(bInfo.rules, [])

        env.assertTrue(r.execute_command('RENAME', 'a2{1}', 'a3{1}'))
        bInfo = TSInfo(r.execute_command('TS.INFO', 'b{1}'))
        env.assertEqual(bInfo.sourceKey, b'a3{1}')
        env.assertEqual(bInfo.rules, [])


def test_rename_dst(env):

    with env.getClusterConnectionIfNeeded() as r:

        assert r.execute_command('TS.CREATE', 'a{2}')
        assert r.execute_command('TS.CREATE', 'b{2}')
        assert r.execute_command('TS.CREATERULE', 'a{2}', 'b{2}', 'AGGREGATION', 'AVG', 5000)

        env.assertTrue(r.execute_command('RENAME', 'b{2}', 'b1{2}'))
        aInfo = TSInfo(r.execute_command('TS.INFO', 'a{2}'))
        env.assertEqual(aInfo.sourceKey, None)
        env.assertEqual(aInfo.rules[0][0], b'b1{2}')

        assert r.execute_command('TS.CREATE', 'c{2}')
        assert r.execute_command('TS.CREATERULE', 'a{2}', 'c{2}', 'AGGREGATION', 'COUNT', 2000)

        assert r.execute_command('TS.CREATE', 'd{2}')
        assert r.execute_command('TS.CREATERULE', 'a{2}', 'd{2}', 'AGGREGATION', 'SUM', 3000)

        env.assertTrue(r.execute_command('RENAME', 'c{2}', 'c1{2}'))
        aInfo = TSInfo(r.execute_command('TS.INFO', 'a{2}'))
        env.assertEqual(aInfo.sourceKey, None)
        env.assertEqual(aInfo.rules[0][0], b'b1{2}')
        env.assertEqual(aInfo.rules[1][0], b'c1{2}')
        env.assertEqual(aInfo.rules[2][0], b'd{2}')
