--TEST--
pdo_duckdb: DuckDB 2.0 appender casts strings into TIMESTAMPTZ_NS and pre-checks scalars that cannot cast
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
$db->exec('CREATE TABLE t (id INTEGER, x TIMESTAMPTZ_NS, xs TIMESTAMPTZ_NS[], st STRUCT(t TIMESTAMPTZ_NS))');
$ap = $db->duckdbAppender('t');
$ap->appendRow(1, '2020-01-01 00:00:00.123456789+02', ['2020-01-01 00:00:00.5+00', null], ['t' => 'infinity']);
$ap->appendRow(2, '1969-12-31 23:59:59.999999999+00', [], ['t' => null]);
$ap->appendRow(3, null, null, null);

// Each rejected scalar fails before the native append, so the appender and
// its buffered rows survive.
foreach ([
    'unparseable' => [4, 'not a timestamp', [], null],
    'integer' => [4, 5, [], null],
    'float' => [4, 1.5, [], null],
    'nested bad string' => [4, null, ['nope'], null],
    'nested integer' => [4, null, [7], null],
    'struct bad string' => [4, null, null, ['t' => 'nope']],
] as $label => $row) {
    try {
        $ap->appendRow(...$row);
        echo "$label: appended\n";
    } catch (Throwable $e) {
        echo "$label: ", get_class($e), ': ', strtok($e->getMessage(), "\n"), "\n";
    }
}
$ap->close();

foreach ($db->query('SELECT id, x, xs, st FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) as $row) {
    echo json_encode($row), "\n";
}
?>
--EXPECT--
unparseable: PDOException: Failed to append value: Conversion Error: invalid timestamp field format: "not a timestamp", expected format is (YYYY-MM-DD HH:MM[:SS[.US]][±HH[:MM[:SS]]| ZONE])
integer: PDOException: Failed to append value: Conversion Error: Unimplemented type for cast (BIGINT -> TIMESTAMPTZ_NS)
float: PDOException: Failed to append value: Conversion Error: Unimplemented type for cast (DOUBLE -> TIMESTAMPTZ_NS)
nested bad string: ValueError: Pdo\Duckdb\Appender::appendRow(): argument #3 could not be built as a nested value
nested integer: ValueError: Pdo\Duckdb\Appender::appendRow(): argument #3 could not be built as a nested value
struct bad string: ValueError: Pdo\Duckdb\Appender::appendRow(): argument #4 could not be built as a struct
[1,"2019-12-31 22:00:00.123456789+00","['2020-01-01 00:00:00.5+00', NULL]","{'t': infinity}"]
[2,"1969-12-31 23:59:59.999999999+00","[]","{'t': NULL}"]
[3,null,null,null]
