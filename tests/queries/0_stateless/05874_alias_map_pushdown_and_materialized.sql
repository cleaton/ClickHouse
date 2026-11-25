-- Test ALIAS Map pushdown + automatic MATERIALIZED column substitution

SET optimize_map_alias_pushdown = 1;
SET optimize_use_materialized_columns = 1;

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
SELECT ts, attrs['service'] FROM t_logs WHERE ts = 1 SETTINGS optimize_map_alias_pushdown = 0, optimize_use_materialized_columns = 0;

DROP VIEW IF EXISTS v_logs;
DROP TABLE IF EXISTS t_logs_right;
DROP TABLE IF EXISTS t_mem_mat;
DROP TABLE IF EXISTS t_logs;
DROP TABLE IF EXISTS t_split_map;
DROP TABLE IF EXISTS t_merged_map;
