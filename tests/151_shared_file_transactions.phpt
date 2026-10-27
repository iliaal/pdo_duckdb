--TEST--
pdo_duckdb: simultaneous file connections share committed writes and isolate transactions
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$path = __DIR__ . '/151_shared.duckdb';
@unlink($path);
@unlink($path . '.wal');
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $path, null, null, $options);
$a->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, marker VARCHAR)');
$a->exec("INSERT INTO t VALUES (1, 'first')");
$b = new PDO('duckdb:' . $path, null, null, $options);
$rows = static fn(PDO $db): string => implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));

$a->exec("INSERT INTO t VALUES (2, 'a')");
echo 'b_after_a=', $rows($b), "\n";
$b->exec("INSERT INTO t VALUES (3, 'b')");
echo 'a_after_b=', $rows($a), "\n";

$marker = bin2hex(random_bytes(16));
$a->beginTransaction();
$a->exec('INSERT INTO t VALUES (10, ' . $a->quote($marker) . ')');
echo 'transaction_local=', $a->inTransaction() && !$b->inTransaction() ? 'yes' : 'BAD', "\n";
echo 'uncommitted_hidden=', (int) $b->query('SELECT count(*) FROM t WHERE marker=' . $b->quote($marker))->fetchColumn() === 0 ? 'yes' : 'BAD', "\n";
$a->rollBack();
echo 'rollback_removed=', (int) $a->query('SELECT count(*) FROM t WHERE marker=' . $a->quote($marker))->fetchColumn() === 0 ? 'yes' : 'BAD', "\n";

$b->beginTransaction();
echo 'snapshot_start=', $rows($b), "\n";
$a->exec("INSERT INTO t VALUES (4, 'committed_during_snapshot')");
echo 'snapshot_stable=', $rows($b), "\n";
$b->commit();
echo 'snapshot_released=', $rows($b), "\n";

$a = null;
$b->exec("INSERT INTO t VALUES (5, 'after_other_handle_closed')");
echo 'surviving_handle=', $rows($b), "\n";
$b = null;
$reopened = new PDO('duckdb:' . $path, null, null, $options);
echo 'reopened=', $rows($reopened), "\n";
$reopened = null;
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/151_shared.duckdb');
@unlink(__DIR__ . '/151_shared.duckdb.wal');
?>
--EXPECT--
b_after_a=1,2
a_after_b=1,2,3
transaction_local=yes
uncommitted_hidden=yes
rollback_removed=yes
snapshot_start=1,2,3
snapshot_stable=1,2,3
snapshot_released=1,2,3,4
surviving_handle=1,2,3,4,5
reopened=1,2,3,4,5
