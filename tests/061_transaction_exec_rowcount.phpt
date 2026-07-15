--TEST--
pdo_duckdb: multi-statement transaction exec returns the last row count and preserves partial effects
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER)');

$changed = $db->exec('BEGIN; INSERT INTO t VALUES (1), (2)');
echo "open changed=$changed state=", $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();
echo 'after rollback=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

$changed = $db->exec('BEGIN; INSERT INTO t VALUES (3), (4); COMMIT');
echo "closed changed=$changed state=", $db->inTransaction() ? 'active' : 'idle', "\n";
echo 'after commit=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

try {
    $db->exec('BEGIN; INSERT INTO t VALUES (5); COMMIT; INSERT INTO missing_table VALUES (6)');
    echo "failing batch accepted (BUG)\n";
} catch (PDOException $e) {
    echo 'failure state=', $db->inTransaction() ? 'active' : 'idle', "\n";
}
echo 'after failure=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
open changed=2 state=active
after rollback=0
closed changed=0 state=idle
after commit=2
failure state=idle
after failure=3
