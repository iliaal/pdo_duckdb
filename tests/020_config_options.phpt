--TEST--
pdo_duckdb: connection config via DSN options and PDO::DUCKDB_ATTR_CONFIG
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$file = __DIR__ . '/020_config.duckdb';
@unlink($file);

// seed a file database
$w = new PDO("duckdb:$file");
$w->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$w->exec('CREATE TABLE t (id INTEGER)');
$w->exec('INSERT INTO t VALUES (1)');
$w = null;

// constants exist and are driver-specific (>= 1000)
var_dump(PDO::DUCKDB_ATTR_CONFIG >= 1000, PDO::DUCKDB_ATTR_UNBUFFERED >= 1000);

// 1. access_mode=read_only via DSN: reads work, writes are rejected
$ro = new PDO("duckdb:$file;access_mode=read_only");
$ro->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
echo 'read_only count=', $ro->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
try {
    $ro->exec('INSERT INTO t VALUES (2)');
    echo "write allowed (BUG)\n";
} catch (PDOException $e) {
    echo "read_only rejects write\n";
}
$ro = null;

// 2. config via the PDO::DUCKDB_ATTR_CONFIG array
$c = new PDO('duckdb::memory:', null, null, [
    PDO::DUCKDB_ATTR_CONFIG => ['threads' => 2, 'memory_limit' => '512MB'],
]);
$c->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
echo 'threads=', $c->query("SELECT current_setting('threads')")->fetchColumn(), "\n";
$c = null;

// 3. an invalid option name fails the connection with a clear error
try {
    new PDO('duckdb::memory:;not_a_real_option=1');
    echo "bad option accepted (BUG)\n";
} catch (PDOException $e) {
    echo 'bad option: ', str_contains($e->getMessage(), 'not_a_real_option') ? 'named in error' : 'generic', "\n";
}

// 4. a malformed DSN segment (no '=') is rejected
try {
    new PDO('duckdb::memory:;justakey');
    echo "malformed accepted (BUG)\n";
} catch (PDOException $e) {
    echo "malformed DSN rejected\n";
}

// 5. a malformed segment AFTER a valid option is also rejected (and must not leak
// the partially-built config — exercised under ASan).
try {
    new PDO('duckdb::memory:;threads=2;justakey');
    echo "malformed-after-valid accepted (BUG)\n";
} catch (PDOException $e) {
    echo "malformed-after-valid rejected\n";
}
?>
--EXPECT--
bool(true)
bool(true)
read_only count=1
read_only rejects write
threads=2
bad option: named in error
malformed DSN rejected
malformed-after-valid rejected
--CLEAN--
<?php
@unlink(__DIR__ . '/020_config.duckdb');
?>
