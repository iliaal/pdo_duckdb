--TEST--
pdo_duckdb: open_basedir disables DuckDB SQL-level external file access
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// With open_basedir in effect, the driver opens DuckDB with external access
// disabled, so SQL-level file reads (read_csv/COPY/ATTACH/httpfs) are denied.
try {
    $db->query("SELECT * FROM read_csv('/etc/hostname')")->fetchAll();
    echo "NOT BLOCKED\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}
?>
--EXPECT--
bool(true)
