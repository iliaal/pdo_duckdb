--TEST--
pdo_duckdb: a file log sink rejected by the open_basedir sandbox does not abort at handle close
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$sink = __DIR__ . '/114_log_sink.d';
$db->exec("CALL enable_logging(storage='file', storage_path='$sink')");
try {
    $db->query('SELECT 1');
    echo "no error\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}
$db = null;
var_dump(file_exists($sink));
echo "closed\n";
?>
--EXPECT--
bool(true)
bool(false)
closed
