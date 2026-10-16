--TEST--
pdo_duckdb: a failing file log sink surfaces as PDOException and the handle closes without aborting
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// A path under a regular file is unwritable for every user, root included.
$blocker = __DIR__ . '/113_log_sink.notadir';
file_put_contents($blocker, '');
$sink = $blocker . '/sub';

function open_logging(string $sink, array $options = []): PDO
{
    $db = new PDO('duckdb::memory:', null, null, $options + [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $db->exec("CALL enable_logging(storage='file', storage_path='$sink')");
    return $db;
}

function attempt(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: no error\n";
    } catch (PDOException $e) {
        echo "$label: ", str_contains($e->getMessage(), 'IO Error') ? 'PDOException IO Error' : $e->getMessage(), "\n";
    }
}

$db = open_logging($sink);
attempt('buffered query', fn() => $db->query('SELECT 1'));
$db = null;
echo "closed buffered\n";

$db = open_logging($sink, [PDO::DUCKDB_ATTR_UNBUFFERED => true]);
attempt('unbuffered query', fn() => $db->query('SELECT 1'));
$db = null;
echo "closed unbuffered\n";

$db = open_logging($sink);
$stmt = $db->prepare('SELECT ?');
attempt('prepared execute', fn() => $stmt->execute([1]));
$stmt = null;
$db = null;
echo "closed prepared\n";

// No-regression check only: with a transaction open, PDO core rolls back
// before the closer runs, and that ROLLBACK already ends the stale query.
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->beginTransaction();
$db->exec('CREATE TABLE t (i INTEGER)');
$db->exec("CALL enable_logging(storage='file', storage_path='$sink')");
attempt('rollback-first close', fn() => $db->query('SELECT 1'));
$db = null;
echo "closed after rollback-first path\n";

$db = open_logging($sink, [PDO::ATTR_PERSISTENT => true]);
attempt('persistent query', fn() => $db->query('SELECT 1'));
$db = null;
echo "released persistent\n";
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/113_log_sink.notadir');
?>
--EXPECT--
buffered query: PDOException IO Error
closed buffered
unbuffered query: PDOException IO Error
closed unbuffered
prepared execute: PDOException IO Error
closed prepared
rollback-first close: PDOException IO Error
closed after rollback-first path
persistent query: PDOException IO Error
released persistent
