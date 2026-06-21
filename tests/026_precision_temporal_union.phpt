--TEST--
pdo_duckdb: TIMESTAMP_S/MS/NS, TIME_NS, UNION, and VARINT decode (no silent NULL)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$row = $db->query("SELECT
    TIMESTAMP_S '2026-01-01 12:00:00'                       AS ts_s,
    TIMESTAMP_MS '2026-01-01 12:00:00.123'                  AS ts_ms,
    TIMESTAMP_NS '2026-01-01 12:00:00.123456789'            AS ts_ns,
    CAST('12:34:56.123456789' AS TIME_NS)                   AS t_ns,
    union_value(num := 2)::UNION(num INTEGER, str VARCHAR)  AS u_num,
    union_value(str := 'hi')::UNION(num INTEGER, str VARCHAR) AS u_str
")->fetch(PDO::FETCH_ASSOC);

foreach ($row as $k => $v) {
    echo "$k=", var_export($v, true), "\n";
}

// VARINT (arbitrary precision): the driver's decode must match DuckDB's own cast.
foreach (['0', '255', '-7', '170141183460469231731687303715884105729',
          '-123456789012345678901234567890'] as $n) {
    $r = $db->query("SELECT CAST('$n' AS VARINT) AS got,
                            CAST(CAST('$n' AS VARINT) AS VARCHAR) AS want")->fetch(PDO::FETCH_ASSOC);
    echo "varint $n: ", ($r['got'] === $r['want'] ? 'ok' : "MISMATCH {$r['got']}"), "\n";
}
?>
--EXPECT--
ts_s='2026-01-01 12:00:00'
ts_ms='2026-01-01 12:00:00.123'
ts_ns='2026-01-01 12:00:00.123456789'
t_ns='12:34:56.123456789'
u_num='2'
u_str='hi'
varint 0: ok
varint 255: ok
varint -7: ok
varint 170141183460469231731687303715884105729: ok
varint -123456789012345678901234567890: ok
--CLEAN--
<?php
?>
