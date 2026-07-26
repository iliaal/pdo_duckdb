--TEST--
pdo_duckdb: beginTransaction escalates open_basedir (sticky log_query_path)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Same sticky-writer hazard as 064, but escalate must run on the PDO
// transaction API (duckdb_simple_exec), not only on exec/query/prepare.
$log = __DIR__ . '/067_queries_' . getmypid() . '.sql';
@unlink($log);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET log_query_path = ' . $db->quote($log));
$db->exec("SELECT 'before_sandbox' AS x");

ini_set('open_basedir', __DIR__);

// Escalate via beginTransaction — must clear sticky writer before BEGIN.
$db->beginTransaction();
$db->exec("SELECT 'after_sandbox' AS y");
$db->commit();

$contents = file_get_contents($log);
echo str_contains($contents, 'before_sandbox') ? "pre_logged=yes\n" : "pre_logged=no\n";
echo str_contains($contents, 'after_sandbox') ? "post_logged=BAD\n" : "post_logged=no\n";
echo str_contains($contents, "SELECT 'after_sandbox'") ? "post_sql=BAD\n" : "post_sql=no\n";
@unlink($log);
?>
--EXPECT--
pre_logged=yes
post_logged=no
post_sql=no
