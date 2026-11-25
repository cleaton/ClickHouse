-- Test ALIAS Map pushdown + automatic MATERIALIZED column substitution

SET optimize_map_alias_pushdown = 1;
SET optimize_map_materialized_columns = 1;

-- 1. Use Case 1a: Migrating / merging maps A, B -> C with prefix slicing
DROP TABLE IF EXISTS t_merged_map;
CREATE TABLE t_merged_map
(
    id UInt64,
    C Map(String, String),
    A Map(String, String) ALIAS mapApply((k, v) -> (substring(k, 3), v), mapFilter((k, v) -> startsWith(k, 'a.'), C)),
    B Map(String, String) ALIAS mapApply((k, v) -> (substring(k, 3), v), mapFilter((k, v) -> startsWith(k, 'b.'), C))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_merged_map VALUES (1, {'a.foo': '10', 'a.bar': '20', 'b.foo': '30'}), (2, {'b.baz': '40'});

SELECT '=== UC1a: merged map queries ===';
SELECT id, A['foo'], A['bar'], B['foo'], B['baz'], A['missing'] FROM t_merged_map ORDER BY id;
SELECT id, has(A, 'foo'), mapContainsKey(A, 'baz'), has(B, 'foo') FROM t_merged_map ORDER BY id;

SELECT '=== UC1a: dynamic key column ===';
SELECT id, k, A[k], has(A, k) FROM t_merged_map ARRAY JOIN ['foo', 'bar', 'baz'] AS k ORDER BY id, k;

SELECT '=== UC1a: whole-map access still works ===';
SELECT id, mapSort(A), mapSort(B) FROM t_merged_map ORDER BY id;

SELECT '=== UC1a: verify mapApply/mapFilter pushed down ===';
SELECT count() >= 1 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT A['foo'] FROM t_merged_map) WHERE explain LIKE '%C%' AND explain LIKE '%a.foo%');
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT A['foo'] FROM t_merged_map) WHERE explain LIKE '%mapApply%');

-- 2. Use Case 1b: Splitting hot_attrs and cold_attrs with unused-column pruning
DROP TABLE IF EXISTS t_split_map;
CREATE TABLE t_split_map
(
    id UInt64,
    hot_attrs Map(String, String),
    cold_attrs Map(String, String),
    attrs Map(String, String) ALIAS mapUpdate(
        mapFilter((k, v) -> k NOT IN ('service', 'status'), cold_attrs),
        mapFilter((k, v) -> k IN ('service', 'status'), hot_attrs)
    )
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO t_split_map VALUES
    (1, {'service': 'auth', 'status': '200', 'stale': 'ignore'}, {'trace_id': 't1', 'service': 'old_auth'}),
    (2, {'service': 'pay'}, {'trace_id': 't2', 'user_agent': 'curl'});

SELECT '=== UC1b: split map queries ===';
SELECT id, attrs['service'], attrs['status'], attrs['trace_id'], has(attrs, 'stale'), has(attrs, 'service'), has(attrs, 'trace_id')
FROM t_split_map ORDER BY id;

SELECT '=== UC1b: verify unused map pruned ===';
-- Querying 'service' must read hot_attrs and NOT cold_attrs; querying 'trace_id' must read cold_attrs and NOT hot_attrs
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT attrs['service'] FROM t_split_map) WHERE explain LIKE '%cold_attrs%');
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT attrs['trace_id'] FROM t_split_map) WHERE explain LIKE '%hot_attrs%');

-- 3. Use Case 2: Dynamic MATERIALIZED columns without editing ALIAS, + masking + PK/bloom_filter
DROP TABLE IF EXISTS t_logs;
CREATE TABLE t_logs
(
    ts UInt32,
    raw_attrs Map(String, String),
    __mat_service String MATERIALIZED raw_attrs['service'],
    attrs Map(String, String) ALIAS mapFilter((k, v) -> k != 'ssn', raw_attrs),
    INDEX idx_mat_svc __mat_service TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY (ts, __mat_service)
SETTINGS index_granularity = 2;

INSERT INTO t_logs VALUES
    (1, {'service': 'checkout', 'http.url': '/pay', 'ssn': '111-11', 'status': '200'}),
    (2, {'service': 'auth', 'http.url': '/login', 'ssn': '222-22', 'status': '200'}),
    (3, {'service': 'search', 'http.url': '/find', 'ssn': '333-33', 'status': '404'}),
    (4, {'service': 'checkout', 'http.url': '/confirm', 'ssn': '444-44', 'status': '500'});

SELECT '=== UC2: initial materialized + masked query ===';
SELECT ts, attrs['service'], attrs['http.url'], attrs['ssn'], has(attrs, 'ssn') FROM t_logs WHERE attrs['service'] = 'checkout' ORDER BY ts;

SELECT '=== UC2: verify __mat_service used without reading raw_attrs ===';
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT ts FROM t_logs WHERE attrs['service'] = 'checkout') WHERE explain LIKE '%raw_attrs%');
SELECT count() >= 1 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT ts FROM t_logs WHERE attrs['service'] = 'checkout') WHERE explain LIKE '%__mat_service%');
SELECT count() = 1 FROM (SELECT explain FROM (EXPLAIN indexes = 1 SELECT ts FROM t_logs WHERE attrs['service'] = 'checkout') WHERE explain LIKE '%PrimaryKey%');

SELECT '=== UC2: add __mat_http_url WITHOUT changing attrs ALIAS ===';
ALTER TABLE t_logs ADD COLUMN __mat_http_url String MATERIALIZED raw_attrs['http.url'] SETTINGS mutations_sync = 1;
ALTER TABLE t_logs MATERIALIZE COLUMN __mat_http_url SETTINGS mutations_sync = 1;

SELECT count() >= 1 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT attrs['http.url'] FROM t_logs) WHERE explain LIKE '%__mat_http_url%');
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT attrs['http.url'] FROM t_logs) WHERE explain LIKE '%raw_attrs%');
SELECT ts, attrs['http.url'] FROM t_logs ORDER BY ts;

SELECT '=== UC2: drop __mat_http_url falls back cleanly ===';
ALTER TABLE t_logs DROP COLUMN __mat_http_url SETTINGS mutations_sync = 1;
SELECT ts, attrs['http.url'] FROM t_logs ORDER BY ts;

SELECT '=== UC2: type mismatch guard prevents silent breakage ===';
-- Mismatched UInt16 MATERIALIZED column must NOT replace String lookup raw_attrs['status']
ALTER TABLE t_logs ADD COLUMN __mat_status_u16 UInt16 MATERIALIZED toUInt16OrZero(raw_attrs['status']) SETTINGS mutations_sync = 1;
ALTER TABLE t_logs ADD COLUMN __mat_status_direct UInt16 MATERIALIZED raw_attrs['status'] SETTINGS mutations_sync = 1;
SELECT ts, startsWith(attrs['status'], '2') FROM t_logs ORDER BY ts;

-- 4. Memory engine must NOT substitute MATERIALIZED columns (missing values return type default on old blocks)
DROP TABLE IF EXISTS t_mem_mat;
CREATE TABLE t_mem_mat (id UInt64, raw_attrs Map(String, String)) ENGINE = Memory;
INSERT INTO t_mem_mat VALUES (1, {'service': 'legacy_mem'});
ALTER TABLE t_mem_mat ADD COLUMN __mat_service String MATERIALIZED raw_attrs['service'];
SELECT id, raw_attrs['service'], __mat_service FROM t_mem_mat ORDER BY id;

-- 5. Complex SQL shapes: JOIN, SEMI JOIN, ARRAY JOIN, GROUP BY, PREWHERE, CTE, VIEW, Lambda shadowing
DROP TABLE IF EXISTS t_logs_right;
CREATE TABLE t_logs_right
(
    ts UInt32,
    raw_attrs Map(String, String),
    attrs Map(String, String) ALIAS mapFilter((k, v) -> k != 'secret_r', raw_attrs)
)
ENGINE = MergeTree
ORDER BY ts;
INSERT INTO t_logs_right VALUES (1, {'service': 'right_svc', 'secret_r': 'hidden'}), (5, {'service': 'unmatched', 'secret_r': 'x'});

SELECT '=== Complex SQL: JOIN / PREWHERE / GROUP BY / VIEW / CTE / Lambda ===';
SELECT attrs['service'], attrs['http.url'], attrs['ssn'] FROM t_logs PREWHERE attrs['service'] = 'checkout' ORDER BY ts;
SELECT attrs['service'] AS svc, count() AS cnt FROM t_logs GROUP BY svc ORDER BY svc;
SELECT t_logs.ts, r.attrs['service'], r.attrs['secret_r'] FROM t_logs LEFT JOIN t_logs_right AS r ON t_logs.ts = r.ts ORDER BY t_logs.ts SETTINGS join_use_nulls = 1;
SELECT t_logs.ts, t_logs.attrs[r.k] FROM t_logs INNER JOIN (SELECT 1 AS ts, 'service' AS k UNION ALL SELECT 2 AS ts, 'ssn' AS k) AS r ON t_logs.ts = r.ts ORDER BY t_logs.ts;
SELECT ts, attrs['service'] FROM t_logs WHERE ts IN (1, 2) ORDER BY ts SETTINGS single_join_prefer_left_table = 1;
SELECT ts, m['service'] FROM t_logs ARRAY JOIN [map('service', 'shadow_val')] AS m ORDER BY ts;
SELECT ts, arrayMap(attrs -> attrs['service'], [map('service', 'lambda_val')]) FROM t_logs WHERE ts = 1;

WITH cte AS (SELECT ts, attrs FROM t_logs WHERE ts <= 2)
SELECT ts, attrs['service'], attrs['ssn'] FROM cte ORDER BY ts;

DROP VIEW IF EXISTS v_logs;
CREATE VIEW v_logs AS SELECT ts, attrs FROM t_logs;
SELECT ts, attrs['service'], attrs['ssn'] FROM v_logs ORDER BY ts;

-- Verify optimization can be disabled per query cleanly
SELECT ts, attrs['service'] FROM t_logs WHERE ts = 1 SETTINGS optimize_map_alias_pushdown = 0, optimize_map_materialized_columns = 0;

SELECT '=== Hardening: GROUP BY whole map, ROLLUP group_by_use_nulls, PREWHERE nested alias, EXPLAIN SYNTAX ===';
SELECT attrs['service'], count() FROM t_logs GROUP BY attrs['service'] WITH ROLLUP ORDER BY 1 NULLS FIRST, 2 SETTINGS group_by_use_nulls = 1;
SELECT attrs['service'], count() FROM t_logs GROUP BY attrs ORDER BY 1, 2;
SELECT raw_attrs['service'], count() FROM t_logs GROUP BY raw_attrs ORDER BY 1, 2;
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN SYNTAX SELECT attrs['service'] FROM t_logs) WHERE explain LIKE '%raw_attrs%' OR explain LIKE '%__mat_service%');
SELECT ts, x FROM t_logs ARRAY JOIN [raw_attrs['service']] AS x ORDER BY ts;

DROP TABLE IF EXISTS t_logs_nested_pw;
CREATE TABLE t_logs_nested_pw
(
    ts UInt32,
    raw_attrs Map(String, String),
    a1 Map(String, String) ALIAS mapFilter((k, v) -> k != 'ssn', raw_attrs),
    a2 Map(String, String) ALIAS mapFilter((k, v) -> k != 'secret', a1)
)
ENGINE = MergeTree
ORDER BY ts;
INSERT INTO t_logs_nested_pw VALUES (1, {'service': 'ok', 'ssn': '1', 'secret': '2'}), (2, {'service': 'other'});
SELECT ts, a2['service'], a2['ssn'], a2['secret'] FROM t_logs_nested_pw PREWHERE a2['service'] = 'ok' AND a1['service'] = 'ok' ORDER BY ts;
DROP TABLE t_logs_nested_pw;

SELECT '=== Hardening: SummingMergeTree & FINAL must NOT substitute MATERIALIZED columns ===';
DROP TABLE IF EXISTS t_sum_mat;
CREATE TABLE t_sum_mat
(
    id UInt64,
    raw_attrs Map(String, String),
    __mat_service String MATERIALIZED 'stale_sum_value'
)
ENGINE = SummingMergeTree
ORDER BY id;
INSERT INTO t_sum_mat VALUES (1, {'service': 'live_sum'});
SELECT id, raw_attrs['service'] FROM t_sum_mat ORDER BY id;
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT raw_attrs['service'] FROM t_sum_mat) WHERE explain LIKE '%__mat_service%');
DROP TABLE IF EXISTS t_rep_final;
CREATE TABLE t_rep_final
(
    id UInt64,
    raw_attrs Map(String, String),
    __mat_service String MATERIALIZED 'stale_rep_final'
)
ENGINE = ReplacingMergeTree
ORDER BY id;
INSERT INTO t_rep_final VALUES (1, {'service': 'live_rep'});
SELECT count() = 0 FROM (SELECT explain FROM (EXPLAIN actions = 1 SELECT raw_attrs['service'] FROM t_rep_final FINAL) WHERE explain LIKE '%__mat_service%');
DROP TABLE t_rep_final;
DROP TABLE t_sum_mat;

SELECT '=== Hardening: Map(Int64, String) key normalization & PK candidate preference ===';
DROP TABLE IF EXISTS t_int_map_pk;
CREATE TABLE t_int_map_pk
(
    id UInt64,
    raw_attrs Map(Int64, String),
    __mat_non_pk String MATERIALIZED raw_attrs[42],
    __mat_pk String MATERIALIZED raw_attrs[42]
)
ENGINE = MergeTree
ORDER BY (__mat_pk, id);
INSERT INTO t_int_map_pk VALUES (1, {42: 'found_42'}), (2, {7: 'other'});
SELECT id, raw_attrs[toInt64(42)], raw_attrs[toUInt8(42)] FROM t_int_map_pk WHERE raw_attrs[toUInt8(42)] = 'found_42' ORDER BY id;
SELECT count() >= 1 FROM (SELECT explain FROM (EXPLAIN indexes = 1 SELECT id FROM t_int_map_pk WHERE raw_attrs[42] = 'found_42') WHERE explain LIKE '%PrimaryKey%');
DROP TABLE t_int_map_pk;

SELECT '=== Hardening: empty map, Nullable key/pred, dead-branch if, non-boolean and, FixedString, transform_null_in, rand() ===';
DROP TABLE IF EXISTS t_edge_alg;
CREATE TABLE t_edge_alg
(
    id UInt64,
    raw_attrs Map(String, String),
    m_empty Map(Nothing, Nothing) ALIAS map(),
    m_cast_empty Map(String, String) ALIAS CAST(map(), 'Map(String, String)'),
    m_dead Map(String, String) ALIAS mapApply((k, v) -> (k, if(0, toString(intDiv(1, toInt64OrZero(v))), v)), raw_attrs),
    m_null_pred Map(String, String) ALIAS mapFilter((k, v) -> toNullable(k != 'ssn'), raw_attrs),
    m_and_u16 Map(String, String) ALIAS mapFilter((k, v) -> and(1, toUInt16(256)), raw_attrs),
    m_and_u8 Map(String, String) ALIAS mapFilter((k, v) -> and(1, toUInt8(5)), raw_attrs),
    m_fs_in Map(FixedString(4), String) ALIAS mapFilter((k, v) -> k IN (toFixedString('k1', 4), toFixedString('k2', 4)), CAST(map('k1', 'v1', 'k3', 'v3'), 'Map(FixedString(4), String)'))
)
ENGINE = MergeTree
ORDER BY id;
INSERT INTO t_edge_alg VALUES (1, {'service': 'ok', 'ssn': '123', 'not_num': 'abc'});
SELECT m_cast_empty['k'], has(m_cast_empty, 'k'), has(m_empty, 'k') FROM t_edge_alg;
SELECT m_dead['not_num'], attrs['service'], t_edge_alg.m_null_pred['service'], t_edge_alg.m_null_pred['ssn'], has(t_edge_alg.m_null_pred, 'ssn')
FROM t_edge_alg
CROSS JOIN (SELECT map('service', 'x') AS attrs) AS dummy;
SELECT m_null_pred['service'], m_null_pred['ssn'], has(m_null_pred, 'ssn') FROM t_edge_alg;
SELECT m_and_u16['service'], has(m_and_u16, 'service'), m_and_u8['service'], has(m_and_u8, 'service') FROM t_edge_alg;
SELECT m_fs_in[toFixedString('k1', 4)], m_fs_in[toFixedString('k3', 4)] FROM t_edge_alg;
SELECT m_null_pred[CAST(NULL, 'Nullable(String)')], m_null_pred[toNullable('service')] FROM t_edge_alg;
SELECT m_and_u16['service'], has(m_and_u16, 'service') FROM t_edge_alg SETTINGS transform_null_in = 1;
SELECT (m_and_u16[if(rand() >= 0, 'service', 'ssn')] = 'ok') FROM t_edge_alg;
DROP TABLE t_edge_alg;

DROP VIEW IF EXISTS v_logs;
DROP TABLE IF EXISTS t_logs_right;
DROP TABLE IF EXISTS t_mem_mat;
DROP TABLE IF EXISTS t_logs;
DROP TABLE IF EXISTS t_split_map;
DROP TABLE IF EXISTS t_merged_map;
