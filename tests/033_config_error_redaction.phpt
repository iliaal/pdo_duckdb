--TEST--
pdo_duckdb: connection errors redact config values and DSN option tails
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$secret = 'SECRET_VALUE_MUST_NOT_LEAK';

try {
    new PDO("duckdb::memory:;threads=$secret");
    echo "BAD: invalid DSN value accepted\n";
} catch (PDOException $e) {
    echo 'invalid DSN option names key: ', str_contains($e->getMessage(), 'threads') ? 'yes' : 'no', "\n";
    echo 'invalid DSN option hides value: ', str_contains($e->getMessage(), $secret) ? 'no' : 'yes', "\n";
}

try {
    new PDO("duckdb::memory:;$secret");
    echo "BAD: malformed DSN option accepted\n";
} catch (PDOException $e) {
    echo 'malformed DSN option rejected: ', str_contains($e->getMessage(), 'Malformed DuckDB DSN option') ? 'yes' : 'no', "\n";
    echo 'malformed DSN option hides segment: ', str_contains($e->getMessage(), $secret) ? 'no' : 'yes', "\n";
}

try {
    new PDO("duckdb:/tmp/pdo_duckdb_outside.duckdb;threads=$secret");
    echo "BAD: open_basedir path accepted\n";
} catch (PDOException $e) {
    echo 'open_basedir mentions path: ', str_contains($e->getMessage(), '/tmp/pdo_duckdb_outside.duckdb') ? 'yes' : 'no', "\n";
    echo 'open_basedir hides DSN tail: ', str_contains($e->getMessage(), $secret) ? 'no' : 'yes', "\n";
}

try {
    new PDO('duckdb::memory:', null, null, [
        PDO::DUCKDB_ATTR_CONFIG => ['threads' => $secret],
    ]);
    echo "BAD: invalid option array value accepted\n";
} catch (PDOException $e) {
    echo 'array option names key: ', str_contains($e->getMessage(), 'threads') ? 'yes' : 'no', "\n";
    echo 'array option hides value: ', str_contains($e->getMessage(), $secret) ? 'no' : 'yes', "\n";
}

$dir = __DIR__ . '/open_error_' . $secret;
mkdir($dir);
try {
    new PDO('duckdb:' . $dir);
    echo "BAD: directory path accepted as database\n";
} catch (PDOException $e) {
    echo 'open failure generic: ', str_contains($e->getMessage(), 'Unable to open DuckDB database') ? 'yes' : 'no', "\n";
    echo 'open failure hides path: ', str_contains($e->getMessage(), $secret) ? 'no' : 'yes', "\n";
} finally {
    rmdir($dir);
}
?>
--EXPECT--
invalid DSN option names key: yes
invalid DSN option hides value: yes
malformed DSN option rejected: yes
malformed DSN option hides segment: yes
open_basedir mentions path: yes
open_basedir hides DSN tail: yes
array option names key: yes
array option hides value: yes
open failure generic: yes
open failure hides path: yes
