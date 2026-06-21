--TEST--
pdo_duckdb: getColumnMeta reports real DuckDB native types and DECIMAL precision/scale
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$st = $db->query("SELECT
    CAST(1 AS INTEGER)        AS c_int,
    CAST(1 AS BIGINT)         AS c_big,
    CAST(1 AS UBIGINT)        AS c_ubig,
    CAST(1.5 AS DOUBLE)       AS c_dbl,
    CAST(9.99 AS DECIMAL(10,2)) AS c_dec,
    DATE '2026-01-01'         AS c_date,
    TIMESTAMP '2026-01-01 00:00:00' AS c_ts,
    gen_random_uuid()         AS c_uuid,
    CAST([1,2] AS INTEGER[])  AS c_list,
    CAST('x' AS BLOB)         AS c_blob,
    CAST('hi' AS VARCHAR)     AS c_var
");

$n = $st->columnCount();
for ($i = 0; $i < $n; $i++) {
    $m = $st->getColumnMeta($i);
    $extra = $m['native_type'] === 'DECIMAL' ? "|precision={$m['precision']}|scale={$m['scale']}" : '';
    echo "{$m['name']}|{$m['native_type']}|pdo={$m['pdo_type']}{$extra}\n";
}
?>
--EXPECT--
c_int|INTEGER|pdo=1
c_big|BIGINT|pdo=1
c_ubig|UBIGINT|pdo=2
c_dbl|DOUBLE|pdo=2
c_dec|DECIMAL|pdo=2|precision=10|scale=2
c_date|DATE|pdo=2
c_ts|TIMESTAMP|pdo=2
c_uuid|UUID|pdo=2
c_list|LIST|pdo=2
c_blob|BLOB|pdo=3
c_var|VARCHAR|pdo=2
--CLEAN--
<?php
?>
