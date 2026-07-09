--TEST--
pdo_duckdb: open_basedir tightening clears pre-existing DuckDB path allowlists
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// These settings are legal before open_basedir is tightened. DuckDB keeps them
// effective after enable_external_access=false unless the driver clears them first.
$db->exec("SET allowed_directories = ['/etc']");
$db->exec("SET allowed_paths = ['/etc/hostname']");

ini_set('open_basedir', __DIR__);

echo "external_access=", $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
echo "locked=", $db->query("SELECT current_setting('lock_configuration')::VARCHAR")->fetchColumn(), "\n";

try {
    $db->query("SELECT * FROM read_csv('/etc/hostname', header=false)")->fetchAll();
    echo "BAD: allowlist survived sandbox\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'disabled by configuration') ? "blocked\n" : $e->getMessage() . "\n";
}
?>
--EXPECT--
external_access=false
locked=true
blocked
