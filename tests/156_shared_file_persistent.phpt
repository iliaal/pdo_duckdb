--TEST--
pdo_duckdb: persistent and ordinary handles share a file without sharing transaction state
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$path = __DIR__ . '/156_shared.duckdb';
@unlink($path);
@unlink($path . '.wal');
$dsn = 'duckdb:' . $path;
$persistent = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$p = new PDO($dsn, null, null, $persistent);
$p->exec('CREATE TABLE t (id INTEGER)');
$p->exec('INSERT INTO t VALUES (1)');
$a = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('INSERT INTO t VALUES (2)');
$rows = static fn(PDO $db): string => implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
echo 'persistent_sees=', $rows($p), "\n";
$p->beginTransaction();
$p->exec('INSERT INTO t VALUES (99)');
echo 'ordinary_transaction=', $a->inTransaction() ? 'BAD' : 'none', "\n";
echo 'ordinary_sees=', $rows($a), "\n";
$p->rollBack();
$p = null;
$a->exec('INSERT INTO t VALUES (3)');
$p = new PDO($dsn, null, null, $persistent);
echo 'checkout=', $rows($p), "\n";
$a = null;
$p->exec('INSERT INTO t VALUES (4)');
$p = null;
$b = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
echo 'ordinary_reopen=', $rows($b), "\n";
$b = null;
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/156_shared.duckdb');
@unlink(__DIR__ . '/156_shared.duckdb.wal');
?>
--EXPECT--
persistent_sees=1,2
ordinary_transaction=none
ordinary_sees=1,2
checkout=1,2,3
ordinary_reopen=1,2,3,4
