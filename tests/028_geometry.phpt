--TEST--
pdo_duckdb: GEOMETRY decodes to a hex-WKB string (round-trippable)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('INSTALL spatial; LOAD spatial;');

// GEOMETRY has no C-API WKT renderer, so the driver returns the raw WKB as
// uppercase hex. It must round-trip back to the original geometry.
$hex = $db->query('SELECT ST_Point(1, 2) AS g')->fetchColumn();
var_dump($hex);

$stmt = $db->prepare('SELECT ST_AsText(ST_GeomFromHEXWKB(?)) AS w');
$stmt->execute([$hex]);
var_dump($stmt->fetchColumn());
?>
--EXPECT--
string(42) "0101000000000000000000F03F0000000000000040"
string(11) "POINT (1 2)"
--CLEAN--
<?php
?>
