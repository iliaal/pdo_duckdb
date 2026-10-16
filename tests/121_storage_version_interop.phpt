--TEST--
pdo_duckdb: DuckDB 2.0 storage version is selectable through connection config and ATTACH
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
?>
--FILE--
<?php
$dir = sys_get_temp_dir() . '/pdo_duckdb_121';
foreach (glob("$dir/*") ?: [] as $f) {
    @unlink($f);
}
@mkdir($dir);
$opts = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];

function storage_tag(PDO $db, string $name): string
{
    $s = $db->prepare("SELECT tags['storage_version'] FROM duckdb_databases() WHERE database_name = ?");
    $s->execute([$name]);
    return (string) $s->fetchColumn();
}

$db = new PDO("duckdb:$dir/latest.db", null, null, $opts);
$db->exec('CREATE TABLE t AS SELECT 1 AS i');
echo "default: ", storage_tag($db, 'latest'), " ",
    $db->query("SELECT current_setting('storage_compatibility_version')")->fetchColumn(), "\n";

$db->exec("ATTACH '$dir/attached.db' AS attached (STORAGE_VERSION 'v1.5.6')");
$db->exec('CREATE TABLE attached.t AS SELECT 2 AS i');
echo "attach: ", storage_tag($db, 'attached'), "\n";
$db = null;

$db = new PDO("duckdb:$dir/dsn.db;storage_compatibility_version=v1.5.6", null, null, $opts);
$db->exec('CREATE TABLE t AS SELECT 3 AS i');
echo "dsn: ", storage_tag($db, 'dsn'), " ",
    $db->query("SELECT current_setting('storage_compatibility_version')")->fetchColumn(), "\n";
$db = null;

$db = new PDO("duckdb:$dir/config.db", null, null, $opts + [
    PDO::DUCKDB_ATTR_CONFIG => ['storage_compatibility_version' => 'v1.5.6'],
]);
$db->exec('CREATE TABLE t AS SELECT 4 AS i');
echo "config: ", storage_tag($db, 'config'), "\n";
$db = null;

$db = new PDO("duckdb:$dir/dsn.db", null, null, $opts);
echo "reopen: ", storage_tag($db, 'dsn'), " ", $db->query('SELECT i FROM t')->fetchColumn(), "\n";
$db = null;

try {
    new PDO("duckdb:$dir/bad.db;storage_compatibility_version=v9.9.9", null, null, $opts);
    echo "BAD: unknown storage version accepted\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'storage_compatibility_version')
        ? "unknown version rejected\n"
        : "BAD: unexpected error " . $e->getMessage() . "\n";
}
?>
--CLEAN--
<?php
$dir = sys_get_temp_dir() . '/pdo_duckdb_121';
foreach (glob("$dir/*") ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
default: v2.0.0+ latest
attach: v1.5.0+
dsn: v1.5.0+ v1.5.6
config: v1.5.0+
reopen: v1.5.0+ 3
unknown version rejected
