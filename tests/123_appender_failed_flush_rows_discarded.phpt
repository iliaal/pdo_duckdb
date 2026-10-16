--TEST--
pdo_duckdb: rows left buffered by a failed appender flush are not flushed again at destruction
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
?>
--FILE--
<?php
// A failed flush poisons the appender but leaves its rows buffered. Neither
// destroying the appender later nor tearing it down at request shutdown may
// flush them once the conflicting transaction is gone.
$scenario = <<<'PHP'
<?php
function poisoned_appender(string $path, ?object &$placeholder = null): array
{
    $db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    // Frees a lower object handle for the appender, so the shutdown sweep
    // reaches the PDO object first.
    $placeholder = null;
    $db->exec('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY)');
    $db->beginTransaction();
    $db->exec('INSERT INTO t VALUES (1)');
    $ap = $db->duckdbAppender('t');
    $ap->appendRow(1);
    $ap->appendRow(2);
    try {
        $ap->flush();
        echo "BAD: flush succeeded\n";
    } catch (PDOException $e) {
        echo str_contains($e->getMessage(), 'Failed to flush appender') ? "flush failed\n" : $e->getMessage() . "\n";
    }
    return [$db, $ap];
}
PHP;

$dir = __DIR__ . '/123_work';
@mkdir($dir);
file_put_contents($dir . '/scenario.inc', $scenario);
require $dir . '/scenario.inc';

// Refcount release after an explicit rollBack().
[$db, $ap] = poisoned_appender($dir . '/refcount.duckdb');
$db->rollBack();
unset($ap);
echo 'refcount rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
$db = null;

// Request shutdown with the PDO object freed first: PDO rolls back the open
// transaction, then its closer releases the poisoned appender.
$child = <<<'PHP'
<?php
require __DIR__ . '/scenario.inc';
gc_disable();
$placeholder = new stdClass;
$keep = poisoned_appender($argv[1], $placeholder);
if (spl_object_id($keep[1]) >= spl_object_id($keep[0])) {
    echo "BAD handle order\n";
}
$holder = new stdClass;
$holder->self = $holder;
$holder->keep = $keep;
unset($keep);
echo "end of script\n";
PHP;
file_put_contents($dir . '/child.php', $child);

$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$path = $dir . '/shutdown.duckdb';
$proc = proc_open($php . ' ' . escapeshellarg($dir . '/child.php') . ' ' . escapeshellarg($path),
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
fclose($pipes[0]);
$out = stream_get_contents($pipes[1]);
fclose($pipes[1]);
$rc = proc_close($proc);
echo "child rc=$rc\n$out";
$db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
echo 'shutdown rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/123_work';
foreach (glob($dir . '/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
flush failed
refcount rows=0
child rc=0
flush failed
end of script
shutdown rows=0
