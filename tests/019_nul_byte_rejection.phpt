--TEST--
pdo_duckdb: embedded NUL bytes in SQL and appender identifiers are rejected, not truncated
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect() {
    return PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
}

$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (i INTEGER)');

$nul = "SELECT 1\0 INVALID";

// query(): a NUL would truncate "SELECT 1\0 INVALID" to "SELECT 1" — reject it.
try { $db->query($nul); echo "BAD: query truncated+ran\n"; }
catch (\PDOException $e) { var_dump(str_contains($e->getMessage(), 'NUL byte')); }

// prepare()
try { $db->prepare($nul); echo "BAD: prepare truncated+ran\n"; }
catch (\PDOException $e) { var_dump(str_contains($e->getMessage(), 'NUL byte')); }

// exec()
try { $db->exec($nul); echo "BAD: exec truncated+ran\n"; }
catch (\PDOException $e) { var_dump(str_contains($e->getMessage(), 'NUL byte')); }

// appender table name
try { $db->duckdbAppender("t\0bad"); echo "BAD: appender table truncated\n"; }
catch (\ValueError $e) { var_dump(str_contains($e->getMessage(), 'NUL byte')); }

// appender schema name
try { $db->duckdbAppender('t', "main\0bad"); echo "BAD: appender schema truncated\n"; }
catch (\ValueError $e) { var_dump(str_contains($e->getMessage(), 'NUL byte')); }

// A normal statement still works.
var_dump((int) $db->query('SELECT 1')->fetchColumn());
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
int(1)
