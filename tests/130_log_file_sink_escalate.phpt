--TEST--
pdo_duckdb: runtime open_basedir escalate detaches a file log sink opened before the tightening
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
// The sink stays under the future open_basedir so PHP can read it afterward;
// the escalated profile denies DuckDB every path, so the hazard is the sink
// writing on through its already-open file.
$sink = __DIR__ . '/130_log_sink_' . getmypid() . '.d';

$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("CALL enable_logging(storage='file', storage_path=" . $db->quote($sink) . ")");
$db->exec("SELECT 'before_sandbox' AS x");

ini_set('open_basedir', __DIR__);

$db->exec("SELECT 'after_sandbox' AS y");
try {
    $db->exec("CALL enable_logging()");
    if (pdo_duckdb_v2_headers()) {
        echo "BAD: locked logging configuration changed\n";
    }
} catch (PDOException $e) {
    if (!pdo_duckdb_v2_headers() || !str_contains($e->getMessage(), 'enable_logging')
        || !str_contains($e->getMessage(), 'configuration has been locked')) {
        throw $e;
    }
}
$db->exec("SELECT 'after_reenable' AS z");
var_dump($db->query("SELECT current_setting('logging_storage')")->fetchColumn());
$db = null;

$contents = '';
foreach (glob("$sink/*") as $f) {
    $contents .= file_get_contents($f);
}
echo str_contains($contents, 'before_sandbox') ? "pre_logged=yes\n" : "pre_logged=no\n";
echo str_contains($contents, 'after_sandbox') ? "post_logged=BAD\n" : "post_logged=no\n";
echo str_contains($contents, 'after_reenable') ? "reenable_logged=BAD\n" : "reenable_logged=no\n";
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/130_log_sink_*.d') as $d) {
    foreach (glob("$d/*") as $f) {
        @unlink($f);
    }
    @rmdir($d);
}
?>
--EXPECT--
string(6) "memory"
pre_logged=yes
post_logged=no
reenable_logged=no
