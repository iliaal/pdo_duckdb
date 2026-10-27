--TEST--
pdo_duckdb: identically sandboxed file connections share writes without weakening external access
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$path = __DIR__ . '/157_shared.duckdb';
$csv = __DIR__ . '/157_forbidden.csv';
@unlink($path);
@unlink($path . '.wal');
$marker = bin2hex(random_bytes(16));
file_put_contents($csv, "marker\n$marker\n");
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $path, null, null, $options);
$a->exec('CREATE TABLE t (id INTEGER)');
$a->exec('INSERT INTO t VALUES (1)');
$b = new PDO('duckdb:' . $path, null, null, $options);
$a->exec('INSERT INTO t VALUES (2)');
$b->exec('INSERT INTO t VALUES (3)');
foreach ([$a, $b] as $db) {
    echo 'rows=', implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN)), "\n";
    echo 'external=', $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
    try {
        $db->query('SELECT marker FROM read_csv(' . $db->quote($csv) . ', header=true)')->fetchAll();
        echo "sql_read=BAD\n";
    } catch (PDOException $e) {
        echo 'sql_read=', str_contains($e->getMessage(), 'disabled by configuration') ? 'blocked' : 'BAD: ' . $e->getMessage(), "\n";
    }
}
$a = $b = $db = null;
$reopened = new PDO('duckdb:' . $path);
echo 'reopened=', implode(',', $reopened->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN)), "\n";
$reopened = null;
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/157_shared.duckdb');
@unlink(__DIR__ . '/157_shared.duckdb.wal');
@unlink(__DIR__ . '/157_forbidden.csv');
?>
--EXPECT--
rows=1,2,3
external=false
sql_read=blocked
rows=1,2,3
external=false
sql_read=blocked
reopened=1,2,3
