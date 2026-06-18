--TEST--
pdo_duckdb: nested (LIST/ARRAY/STRUCT/MAP) and extended (UUID/TZ/ENUM/BIT) types
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$row = $db->query("SELECT
    [1, 2, 3]::INTEGER[]                              AS list_int,
    ['a', 'b']::VARCHAR[]                             AS list_str,
    [10, NULL, 30]::INTEGER[]                         AS list_null,
    [1, 2, 3]::INTEGER[3]                             AS arr,
    {'x': 1, 'y': 'hi'}                               AS strct,
    MAP {'a': 1, 'b': 2}                              AS m,
    [{'k': 1}, {'k': 2}]                              AS nested,
    '12345678-1234-5678-1234-567812345678'::UUID     AS uuid,
    TIMESTAMPTZ '2026-06-18 12:34:56+00'             AS tstz,
    TIMETZ '12:34:56+02'                             AS timetz,
    INTERVAL 90 SECONDS                              AS ival,
    'happy'::ENUM('happy', 'sad')                    AS enum,
    '101'::BIT                                        AS bits
")->fetch(PDO::FETCH_ASSOC);

foreach ($row as $k => $v) {
    printf("%-9s %s %s\n", $k, gettype($v), $v);
}
?>
--EXPECT--
list_int  string [1, 2, 3]
list_str  string [a, b]
list_null string [10, NULL, 30]
arr       string [1, 2, 3]
strct     string {'x': 1, 'y': hi}
m         string {a=1, b=2}
nested    string [{'k': 1}, {'k': 2}]
uuid      string 12345678-1234-5678-1234-567812345678
tstz      string 2026-06-18 12:34:56+00
timetz    string 12:34:56+02
ival      string 00:01:30
enum      string happy
bits      string 101
