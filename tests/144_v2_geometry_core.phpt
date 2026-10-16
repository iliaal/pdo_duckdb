--TEST--
pdo_duckdb: DuckDB 2.0 core GEOMETRY fetches as hex WKB, keeps its metadata, and appends from WKT
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
// No spatial extension: GEOMETRY, its WKT/WKB casts, and CRS-typed GEOMETRY
// are core in DuckDB 2.0.
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("SET autoload_known_extensions=false");

$st = $db->query("SELECT 'POINT(1 2)'::GEOMETRY AS g, 'POINT(1 2)'::GEOMETRY('OGC:CRS84') AS gc,
    ['POINT(1 2)'::GEOMETRY, NULL] AS gl, {'g': 'LINESTRING(0 0, 1 1)'::GEOMETRY} AS gs, NULL::GEOMETRY AS gn");
for ($i = 0; $i < $st->columnCount(); $i++) {
    $m = $st->getColumnMeta($i);
    echo $m['name'], ': ', $m['native_type'], ' pdo_type=', $m['pdo_type'], "\n";
}
$row = $st->fetch(PDO::FETCH_ASSOC);
echo json_encode($row), "\n";

// The hex WKB round-trips through a core BLOB -> GEOMETRY cast.
$rt = $db->prepare('SELECT CAST(CAST(from_hex(?) AS GEOMETRY) AS VARCHAR)');
$rt->execute([$row['g']]);
var_dump($rt->fetchColumn());
var_dump($db->query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'spatial' AND loaded")->fetchColumn());

$db->exec('CREATE TABLE g (id INTEGER, geom GEOMETRY, geoms GEOMETRY[])');
$ap = $db->duckdbAppender('g');
$ap->appendRow(1, 'POINT(3 4)', ['POINT(5 6)', null]);
$ap->appendRow(2, null, []);
$ap->close();
foreach ($db->query('SELECT id, geom, geoms, CAST(geom AS VARCHAR) AS wkt FROM g ORDER BY id')->fetchAll(PDO::FETCH_NUM) as $r) {
    echo json_encode($r), "\n";
}
?>
--EXPECT--
g: GEOMETRY pdo_type=2
gc: GEOMETRY pdo_type=2
gl: LIST pdo_type=2
gs: STRUCT pdo_type=2
gn: GEOMETRY pdo_type=2
{"g":"0101000000000000000000F03F0000000000000040","gc":"0101000000000000000000F03F0000000000000040","gl":"[0101000000000000000000F03F0000000000000040, NULL]","gs":"{'g': 01020000000200000000000000000000000000000000000000000000000000F03F000000000000F03F}","gn":null}
string(11) "POINT (1 2)"
int(0)
[1,"010100000000000000000008400000000000001040","[010100000000000000000014400000000000001840, NULL]","POINT (3 4)"]
[2,null,"[]",null]
