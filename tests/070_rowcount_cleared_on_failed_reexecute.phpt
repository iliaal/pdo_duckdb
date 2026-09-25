--TEST--
pdo_duckdb: failed re-execute zeros rowCount after a prior success
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR)');

$st = $db->prepare('INSERT INTO t VALUES (?, ?)');
$st->execute([1, 'a']);
echo "first_rowcount=" . $st->rowCount() . "\n";

// An extra EXEC_PRE parameter fails before the driver executer can reset state.
$bindOk = $st->execute([2, 'b', 999]);
echo "bind_failure_ok=" . var_export($bindOk, true) . "\n";
echo "bind_failure_rowcount=" . $st->rowCount() . "\n";
echo "rows_after_bind_failure=" . $db->query('SELECT count(*) FROM t')->fetchColumn() . "\n";

// Constraint failure on re-execute must not leave the previous rowCount.
$ok = $st->execute([1, 'b']);
echo "second_ok=" . var_export($ok, true) . "\n";
echo "second_rowcount=" . $st->rowCount() . "\n";
?>
--EXPECT--
first_rowcount=1
bind_failure_ok=false
bind_failure_rowcount=0
rows_after_bind_failure=1
second_ok=false
second_rowcount=0
