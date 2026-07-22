--TEST--
pdo_duckdb: runtime open_basedir escalate closes sticky log_query_path writer
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Keep the log under the future open_basedir so PHP can still read it after
// escalate. The bug is the sticky DuckDB writer continuing after sandbox, not
// the log path relative to basedir.
$log = __DIR__ . '/064_queries_' . getmypid() . '.sql';
@unlink($log);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET log_query_path = ' . $db->quote($log));
$db->exec("SELECT 'before_sandbox' AS x");

ini_set('open_basedir', __DIR__);

// Escalate on next SQL; must clear the sticky writer before lock.
$db->exec("SELECT 'after_sandbox' AS y");

$contents = file_get_contents($log);
echo str_contains($contents, 'before_sandbox') ? "pre_logged=yes\n" : "pre_logged=no\n";
// Post-sandbox user query must not appear (escalate SET lines may, if they
// ran before the writer was cleared — our fix clears first).
echo str_contains($contents, 'after_sandbox') ? "post_logged=BAD\n" : "post_logged=no\n";
echo str_contains($contents, "SELECT 'after_sandbox'") ? "post_sql=BAD\n" : "post_sql=no\n";
@unlink($log);
?>
--EXPECT--
pre_logged=yes
post_logged=no
post_sql=no
