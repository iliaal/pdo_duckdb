--TEST--
pdo_duckdb: persistent checkout clears the previous request's profiling tree
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, $opts)
    : new PDO('duckdb::memory:', null, null, $opts);

$db->exec("PRAGMA enable_profiling='no_output'");
$db->query('SELECT 41')->fetchColumn();
$p = $db->duckdbLastProfile();
echo 'armed=', is_array($p) && ($p['metrics']['QUERY_NAME'] ?? '') === 'SELECT 41' ? 'yes' : 'no', "\n";
unset($db);

$db2 = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, $opts)
    : new PDO('duckdb::memory:', null, null, $opts);
echo 'checkout=', $db2->duckdbLastProfile() === null ? 'cleared' : 'STALE', "\n";

$db2->exec("PRAGMA enable_profiling='no_output'");
$db2->query('SELECT 42')->fetchColumn();
$p2 = $db2->duckdbLastProfile();
echo 'rearm=', is_array($p2) && ($p2['metrics']['QUERY_NAME'] ?? '') === 'SELECT 42' ? 'yes' : 'no', "\n";
?>
--EXPECT--
armed=yes
checkout=cleared
rearm=yes
