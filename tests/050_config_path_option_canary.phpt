--TEST--
pdo_duckdb: temp_directory config is accepted and observable without open_basedir
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$path = __DIR__ . '/sandbox-temp';
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_CONFIG => ['temp_directory' => $path],
]);

var_dump($db->query("SELECT current_setting('temp_directory')")->fetchColumn() === $path);
?>
--EXPECT--
bool(true)
