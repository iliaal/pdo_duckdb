--TEST--
pdo_duckdb: shared file instances reject incompatible original configurations
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$path = __DIR__ . '/154_shared.duckdb';
@unlink($path);
@unlink($path . '.wal');
$a = new PDO('duckdb:' . $path . ';threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('CREATE TABLE t (id INTEGER)');
$a->exec('INSERT INTO t VALUES (1)');
foreach (['threads=2', 'threads=1;access_mode=read_only'] as $config) {
    try {
        $conflicting = new PDO('duckdb:' . $path . ';' . $config);
        echo "incompatible=BAD\n";
        $conflicting = null;
    } catch (PDOException $e) {
        echo 'incompatible=', $e->getCode() === 2 && $e->errorInfo[0] === '08000' && preg_match('/config|different.*option/i', $e->getMessage()) ? 'rejected' : 'BAD: ' . $e->getMessage(), "\n";
    }
}
$b = new PDO('duckdb:' . $path . ';threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('INSERT INTO t VALUES (2)');
echo 'compatible=', implode(',', $b->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN)), "\n";
$a = $b = null;
$reopened = new PDO('duckdb:' . $path . ';threads=2');
echo 'new_instance_config=', $reopened->query("SELECT current_setting('threads')")->fetchColumn(), "\n";
echo 'durable=', $reopened->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
$reopened = null;
$directory = __DIR__ . '/154_config_case';
@mkdir($directory);
@unlink($directory . '/configuration');
@unlink($directory . '/configuration.wal');
$cwd = getcwd();
chdir($directory);
$a = new PDO('duckdb:configuration;threads=1');
try {
    $conflicting = new PDO('duckdb:configuration;threads=2');
    echo "relative_config_reason=BAD\n";
    $conflicting = null;
} catch (PDOException $e) {
    echo 'relative_config_reason=', $e->getCode() === 2 && $e->errorInfo[0] === '08000' && preg_match('/different.*config/i', $e->getMessage()) ? 'clear' : 'BAD: ' . $e->getMessage(), "\n";
} finally {
    chdir($cwd);
}
$a = null;
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/154_shared.duckdb');
@unlink(__DIR__ . '/154_shared.duckdb.wal');
@unlink(__DIR__ . '/154_config_case/configuration');
@unlink(__DIR__ . '/154_config_case/configuration.wal');
@rmdir(__DIR__ . '/154_config_case');
?>
--EXPECT--
incompatible=rejected
incompatible=rejected
compatible=1,2
new_instance_config=2
durable=2
relative_config_reason=clear
