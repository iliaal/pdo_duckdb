--TEST--
pdo_duckdb: DuckDB 2.0 file log sinks and IO_MODE attaches cannot write outside open_basedir
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) {
    die('skip needs a build against the DuckDB 2.0 headers');
}
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
?>
--FILE--
<?php
$work = __DIR__ . '/134_work_' . getmypid();
$control = "$work/control";
$basedir = "$work/basedir";
$outside = "$work/outside";
foreach ([$work, $control, $basedir, $outside] as $d) {
    mkdir($d);
}
file_put_contents("$outside/secret.csv", "a\n1\n");

function list_files(string $dir): array {
    $files = [];
    foreach (new RecursiveIteratorIterator(new RecursiveDirectoryIterator($dir, FilesystemIterator::SKIP_DOTS)) as $f) {
        $files[] = substr($f->getPathname(), strlen($dir) + 1);
    }
    sort($files);
    return $files;
}

// Positive control without open_basedir: each writer creates its file.
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("CALL enable_logging(storage='file', storage_path='$control/log')");
$db->exec("SELECT 'logged'");
$db->exec("ATTACH '$control/direct.db' AS d (IO_MODE 'DIRECT_IO')");
$db->exec('CREATE TABLE d.t AS SELECT 1 AS v');
$db->exec('DETACH d');
$db->exec("ATTACH '$control/mmap.db' AS m (IO_MODE 'MMAP')");
$db->exec('CREATE TABLE m.t AS SELECT 1 AS v');
$db->exec('DETACH m');
var_dump((int) $db->query("SELECT count(*) FROM read_single_csv_file('$outside/secret.csv')")->fetchColumn());
$db = null;
var_dump(list_files($control) === ['direct.db', 'log/duckdb_log_contexts.csv', 'log/duckdb_log_entries.csv', 'mmap.db']);

$child = <<<'PHP'
<?php
$outside = $argv[1];
function attempt(string $label, string $sql): void {
    // A rejected file log sink leaves the connection failing every later
    // query, so each attempt gets its own handle.
    $db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    try {
        $db->exec($sql);
        $db->query('SELECT 1')->fetchAll();
        echo "$label: ALLOWED\n";
    } catch (PDOException $e) {
        echo "$label: ", preg_match('/disabled by configuration|configuration has been locked/', $e->getMessage()) ? 'denied' : 'other error: ' . strtok($e->getMessage(), "\n"), "\n";
    }
    $db = null;
}
attempt('enable_logging storage_path', "CALL enable_logging(storage='file', storage_path='$outside/log_a')");
attempt('enable_logging storage_config', "CALL enable_logging(storage='file', storage_config={'path': '$outside/log_b'})");
attempt('SET logging_storage', "SET logging_storage='file'");
attempt('SET enable_logging', 'SET enable_logging=true');
attempt('DIRECT_IO attach', "ATTACH '$outside/direct.db' AS d (IO_MODE 'DIRECT_IO')");
attempt('MMAP attach', "ATTACH '$outside/mmap.db' AS m (IO_MODE 'MMAP')");
attempt('read_single_csv_file', "SELECT * FROM read_single_csv_file('$outside/secret.csv')");
echo "child done\n";
PHP;
file_put_contents("$work/child.php", $child);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS')
    . ' -d open_basedir=' . escapeshellarg($basedir . PATH_SEPARATOR . $work);
$proc = proc_open($php . ' ' . escapeshellarg("$work/child.php") . ' ' . escapeshellarg($outside),
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
fclose($pipes[0]);
echo stream_get_contents($pipes[1]);
fclose($pipes[1]);
echo 'child rc: ', proc_close($proc), "\n";
var_dump(list_files($outside));
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/134_work_*') as $work) {
    $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($work, FilesystemIterator::SKIP_DOTS), RecursiveIteratorIterator::CHILD_FIRST);
    foreach ($it as $f) {
        $f->isDir() ? @rmdir($f->getPathname()) : @unlink($f->getPathname());
    }
    @rmdir($work);
}
?>
--EXPECT--
int(1)
bool(true)
enable_logging storage_path: denied
enable_logging storage_config: denied
SET logging_storage: denied
SET enable_logging: denied
DIRECT_IO attach: denied
MMAP attach: denied
read_single_csv_file: denied
child done
child rc: 0
array(1) {
  [0]=>
  string(10) "secret.csv"
}
