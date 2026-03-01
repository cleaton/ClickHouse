-- PROXY column: two-expression design (bare access + ELEMENT lambda)

-- C1: Table creation with ELEMENT
DROP TABLE IF EXISTS test_proxy;
CREATE TABLE test_proxy (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(attrs_lo, attrs_hi)
        ELEMENT (k) -> if(k < 'm', attrs_lo[k], attrs_hi[k]),
    attrs_lo Map(String, String),
    attrs_hi Map(String, String)
) ENGINE = MergeTree() ORDER BY id;

-- C2: INSERT into physical columns directly
INSERT INTO test_proxy (id, attrs_lo, attrs_hi) VALUES (1, {'alpha':'a', 'beta':'b'}, {'zeta':'z', 'omega':'o'});

-- C3: Bare SELECT returns merged map
SELECT attrs FROM test_proxy;

-- C4: Element access with constant key
SELECT attrs['alpha'], attrs['zeta'] FROM test_proxy;

-- C4.1: ELEMENT lambda may call SQL UDF, keeping proxy logic function-based
DROP FUNCTION IF EXISTS proxy_pick_m_03200;
CREATE FUNCTION proxy_pick_m_03200 AS (k, lo, hi) -> if(k < 'm', lo[k], hi[k]);

DROP TABLE IF EXISTS test_proxy_udf_element;
CREATE TABLE test_proxy_udf_element (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(attrs_lo, attrs_hi)
        ELEMENT (k) -> proxy_pick_m_03200(k, attrs_lo, attrs_hi),
    attrs_lo Map(String, String),
    attrs_hi Map(String, String)
) ENGINE = MergeTree() ORDER BY id;

INSERT INTO test_proxy_udf_element (id, attrs_lo, attrs_hi) VALUES (1, {'alpha':'a', 'beta':'b'}, {'zeta':'z', 'omega':'o'});
SELECT attrs['alpha'], attrs['zeta'] FROM test_proxy_udf_element;

-- C5: SELECT * includes proxy column
SELECT * FROM test_proxy;

-- C6: SHOW CREATE TABLE roundtrips with ELEMENT clause
SHOW CREATE TABLE test_proxy FORMAT TSVRaw;

-- C7: Proxy without ELEMENT falls back to arrayElement(bare_expr, key)
DROP TABLE IF EXISTS test_proxy_no_element;
CREATE TABLE test_proxy_no_element (
    id UInt64,
    attrs Map(String, String) PROXY mapConcat(attrs_lo, attrs_hi),
    attrs_lo Map(String, String),
    attrs_hi Map(String, String)
) ENGINE = MergeTree() ORDER BY id;

INSERT INTO test_proxy_no_element (id, attrs_lo, attrs_hi) VALUES (1, {'a':'1'}, {'z':'2'});
SELECT attrs['a'], attrs['z'] FROM test_proxy_no_element;

-- C8: Map sharding — 4 underlying shards, proxy routes by cityHash64(key) % 4
DROP TABLE IF EXISTS test_sharded_map;
CREATE TABLE test_sharded_map (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(mapConcat(s0, s1), mapConcat(s2, s3))
        ELEMENT (k) -> [s0, s1, s2, s3][toUInt8(cityHash64(k) % 4) + 1][k],
    s0 Map(String, String),
    s1 Map(String, String),
    s2 Map(String, String),
    s3 Map(String, String)
) ENGINE = MergeTree() ORDER BY id;

-- Keys distributed by hash: baz->s0, foo/qux->s1, bar->s2
INSERT INTO test_sharded_map (id, s0, s1, s2, s3) VALUES
    (1, {'baz':'BAZ'}, {'foo':'FOO','qux':'QUX'}, {'bar':'BAR'}, {});

-- Element access routes each key to the correct shard
SELECT attrs['foo'], attrs['bar'], attrs['baz'], attrs['qux'] FROM test_sharded_map;

-- Bare access merges all shards into one map
SELECT attrs FROM test_sharded_map;

-- C9: ELEMENT lambda with nested lambda that shadows outer parameter name
DROP TABLE IF EXISTS test_proxy_nested_lambda;
CREATE TABLE test_proxy_nested_lambda (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(attrs_lo, attrs_hi)
        ELEMENT (k) -> if(arrayExists((k) -> k = 'a', mapKeys(attrs_lo)), attrs_lo[k], attrs_hi[k]),
    attrs_lo Map(String, String),
    attrs_hi Map(String, String)
) ENGINE = MergeTree() ORDER BY id;

INSERT INTO test_proxy_nested_lambda (id, attrs_lo, attrs_hi) VALUES (1, {'a':'A'}, {'z':'Z'});
SELECT attrs['a'] FROM test_proxy_nested_lambda;

-- C10: index analysis should see routed underlying map expression for proxy element predicate
DROP TABLE IF EXISTS test_proxy_indexes;
CREATE TABLE test_proxy_indexes (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(attrs_lo, attrs_hi)
        ELEMENT (k) -> if(k < 'm', attrs_lo[k], attrs_hi[k]),
    attrs_lo Map(String, String),
    attrs_hi Map(String, String),
    INDEX idx_lo_alpha attrs_lo['alpha'] TYPE set(100) GRANULARITY 1
) ENGINE = MergeTree() ORDER BY id;

INSERT INTO test_proxy_indexes (id, attrs_lo, attrs_hi) VALUES
    (1, {'alpha':'match'}, {'zeta':'z1'}),
    (2, {'alpha':'miss'}, {'zeta':'z2'});

EXPLAIN indexes = 1
SELECT id
FROM test_proxy_indexes
WHERE attrs['alpha'] = 'match';

-- C11: hash-routed proxy element should collapse to concrete shard access for constant key
DROP TABLE IF EXISTS test_proxy_sharded_indexes;
CREATE TABLE test_proxy_sharded_indexes (
    id UInt64,
    attrs Map(String, String) PROXY
        mapConcat(mapConcat(s0, s1), mapConcat(s2, s3))
        ELEMENT (k) -> [s0, s1, s2, s3][toUInt8(cityHash64(k) % 4) + 1][k],
    s0 Map(String, String),
    s1 Map(String, String),
    s2 Map(String, String),
    s3 Map(String, String),
    INDEX idx_s0_foo s0['foo'] TYPE set(100) GRANULARITY 1,
    INDEX idx_s1_foo s1['foo'] TYPE set(100) GRANULARITY 1,
    INDEX idx_s2_foo s2['foo'] TYPE set(100) GRANULARITY 1,
    INDEX idx_s3_foo s3['foo'] TYPE set(100) GRANULARITY 1
) ENGINE = MergeTree() ORDER BY id;

INSERT INTO test_proxy_sharded_indexes (id, s0, s1, s2, s3) VALUES
    (1, {'baz':'BAZ'}, {'foo':'FOO','qux':'QUX'}, {'bar':'BAR'}, {}),
    (2, {'baz':'BAZ2'}, {'foo':'MISS','qux':'QUX2'}, {'bar':'BAR2'}, {});

EXPLAIN indexes = 1
SELECT id
FROM test_proxy_sharded_indexes
WHERE attrs['foo'] = 'FOO';

DROP TABLE test_proxy;
DROP TABLE test_proxy_no_element;
DROP TABLE test_sharded_map;
DROP TABLE test_proxy_nested_lambda;
DROP TABLE test_proxy_indexes;
DROP TABLE test_proxy_sharded_indexes;
DROP TABLE test_proxy_udf_element;
DROP FUNCTION IF EXISTS proxy_pick_m_03200;
