--TEST--
pdo_duckdb: persistent checkout clears the previous request's profiling tree
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
function query_name(?array $p): ?string
{
    if (!is_array($p)) {
        return null;
    }
    if (pdo_duckdb_v2_headers()) {
        return $p['metrics'] === [] ? (pdo_duckdb_profile_group($p, 'sql')['metrics']['sql'] ?? null) : null;
    }
    return $p['metrics']['QUERY_NAME'] ?? null;
}

$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, $opts)
    : new PDO('duckdb::memory:', null, null, $opts);

$db->exec("PRAGMA enable_profiling='no_output'");
$db->query('SELECT 41')->fetchColumn();
$p = $db->duckdbLastProfile();
echo 'armed=', query_name($p) === 'SELECT 41' ? 'yes' : 'no', "\n";
unset($db);

$db2 = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, $opts)
    : new PDO('duckdb::memory:', null, null, $opts);
echo 'checkout=', $db2->duckdbLastProfile() === null ? 'cleared' : 'STALE', "\n";

$db2->exec("PRAGMA enable_profiling='no_output'");
$db2->query('SELECT 42')->fetchColumn();
$p2 = $db2->duckdbLastProfile();
echo 'rearm=', query_name($p2) === 'SELECT 42' ? 'yes' : 'no', "\n";
?>
--EXPECT--
armed=yes
checkout=cleared
rearm=yes
