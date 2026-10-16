--TEST--
pdo_duckdb: DuckDB 2.0 TIMESTAMPTZ_NS fetches as its UTC string at the top level and as a nested leaf
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("SET TimeZone='UTC'");
$db->exec('CREATE TABLE t (id INTEGER, x TIMESTAMPTZ_NS)');
$db->exec("INSERT INTO t VALUES
    (1, '2020-01-01 00:00:00.123456789+00'),
    (2, '2024-02-29 12:34:56.000000001+02:30'),
    (3, '1969-12-31 23:59:59.999999999+00'),
    (4, '1970-01-01 00:00:00+00'),
    (5, 'infinity'),
    (6, '-infinity'),
    (7, NULL)");
$db->exec('INSERT INTO t SELECT 8, make_timestamp_ns(-9223372036854775808)::TIMESTAMPTZ_NS');
$db->exec('INSERT INTO t SELECT 9, make_timestamp_ns(9223372036854775806)::TIMESTAMPTZ_NS');

$st = $db->query('SELECT x, [x] AS l, {\'t\': x} AS s, MAP {\'k\': x} AS m, [x, x]::TIMESTAMPTZ_NS[2] AS a FROM t ORDER BY id');
foreach (['x', 'l', 's', 'm', 'a'] as $i => $name) {
    $meta = $st->getColumnMeta($i);
    echo "$name: {$meta['native_type']} pdo_type={$meta['pdo_type']}\n";
}
foreach ($st->fetchAll(PDO::FETCH_ASSOC) as $row) {
    echo json_encode($row, JSON_UNESCAPED_SLASHES), "\n";
}

// The rendering is the stored UTC instant, whatever the session TimeZone,
// matching TIMESTAMPTZ.
$db->exec("SET TimeZone='America/New_York'");
var_dump($db->query('SELECT x FROM t WHERE id = 2')->fetchColumn());
var_dump($db->query('SELECT CAST(x AS VARCHAR) FROM t WHERE id = 2')->fetchColumn());
var_dump($db->query("SELECT TIMESTAMPTZ '2024-02-29 12:34:56+02:30'")->fetchColumn());

$un = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::DUCKDB_ATTR_UNBUFFERED => true]);
var_dump($un->query("SELECT '2020-01-01 00:00:00.5+00'::TIMESTAMPTZ_NS")->fetchColumn());
?>
--EXPECT--
x: TIMESTAMP_TZ_NS pdo_type=2
l: LIST pdo_type=2
s: STRUCT pdo_type=2
m: MAP pdo_type=2
a: ARRAY pdo_type=2
{"x":"2020-01-01 00:00:00.123456789+00","l":"['2020-01-01 00:00:00.123456789+00']","s":"{'t': '2020-01-01 00:00:00.123456789+00'}","m":"{k='2020-01-01 00:00:00.123456789+00'}","a":"[2020-01-01 00:00:00.123456789+00, 2020-01-01 00:00:00.123456789+00]"}
{"x":"2024-02-29 10:04:56.000000001+00","l":"['2024-02-29 10:04:56.000000001+00']","s":"{'t': '2024-02-29 10:04:56.000000001+00'}","m":"{k='2024-02-29 10:04:56.000000001+00'}","a":"[2024-02-29 10:04:56.000000001+00, 2024-02-29 10:04:56.000000001+00]"}
{"x":"1969-12-31 23:59:59.999999999+00","l":"['1969-12-31 23:59:59.999999999+00']","s":"{'t': '1969-12-31 23:59:59.999999999+00'}","m":"{k='1969-12-31 23:59:59.999999999+00'}","a":"[1969-12-31 23:59:59.999999999+00, 1969-12-31 23:59:59.999999999+00]"}
{"x":"1970-01-01 00:00:00+00","l":"['1970-01-01 00:00:00+00']","s":"{'t': '1970-01-01 00:00:00+00'}","m":"{k='1970-01-01 00:00:00+00'}","a":"[1970-01-01 00:00:00+00, 1970-01-01 00:00:00+00]"}
{"x":"infinity","l":"[infinity]","s":"{'t': infinity}","m":"{k=infinity}","a":"[infinity, infinity]"}
{"x":"-infinity","l":"[-infinity]","s":"{'t': -infinity}","m":"{k=-infinity}","a":"[-infinity, -infinity]"}
{"x":null,"l":"[NULL]","s":"{'t': NULL}","m":"{k=NULL}","a":"[NULL, NULL]"}
{"x":"1677-09-21 00:12:43.145224192+00","l":"['1677-09-21 00:12:43.145224192+00']","s":"{'t': '1677-09-21 00:12:43.145224192+00'}","m":"{k='1677-09-21 00:12:43.145224192+00'}","a":"[1677-09-21 00:12:43.145224192+00, 1677-09-21 00:12:43.145224192+00]"}
{"x":"2262-04-11 23:47:16.854775806+00","l":"['2262-04-11 23:47:16.854775806+00']","s":"{'t': '2262-04-11 23:47:16.854775806+00'}","m":"{k='2262-04-11 23:47:16.854775806+00'}","a":"[2262-04-11 23:47:16.854775806+00, 2262-04-11 23:47:16.854775806+00]"}
string(32) "2024-02-29 10:04:56.000000001+00"
string(32) "2024-02-29 05:04:56.000000001-05"
string(22) "2024-02-29 10:04:56+00"
string(24) "2020-01-01 00:00:00.5+00"
