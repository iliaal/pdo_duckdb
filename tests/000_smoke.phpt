--TEST--
pdo_duckdb: extension loads, version matches, driver registered
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
var_dump(extension_loaded('pdo_duckdb'));
var_dump(phpversion('pdo_duckdb'));
var_dump(in_array('duckdb', PDO::getAvailableDrivers(), true));
?>
--EXPECT--
bool(true)
string(5) "0.2.0"
bool(true)
