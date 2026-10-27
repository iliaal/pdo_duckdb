--TEST--
pdo_duckdb: a file log sink rejected by the open_basedir sandbox does not abort at handle close
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$sink = __DIR__ . '/114_log_sink.d';
$phase = 'enable';
try {
    $db->exec("CALL enable_logging(storage='file', storage_path='$sink')");
    $phase = 'query';
    $db->query('SELECT 1');
    echo "no error\n";
} catch (PDOException $e) {
    var_dump(pdo_duckdb_v2_headers()
        ? $phase === 'enable' && str_contains($e->getMessage(), 'enable_logging')
            && str_contains($e->getMessage(), 'configuration has been locked')
        : $phase === 'query' && str_contains($e->getMessage(), 'disabled by configuration'));
}
$db = null;
var_dump(file_exists($sink));
echo "closed\n";
?>
--EXPECT--
bool(true)
bool(false)
closed
