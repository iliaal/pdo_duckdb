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

// Wider coverage through a table: empty point, linestring, polygon, 3D,
// GEOMETRY[] column and NULL. Scalar hex must match the WKB encoding exactly
// and round-trip through ST_GeomFromHEXWKB; the list column is checked
// differentially against the engine CAST AS VARCHAR.
$db->exec('CREATE TABLE g (id INTEGER, geom GEOMETRY, geoms GEOMETRY[])');
$db->exec("INSERT INTO g VALUES
    (1, ST_GeomFromText('POINT EMPTY'), NULL),
    (2, ST_GeomFromText('LINESTRING(0 0, 1 1)'), [ST_Point(1, 2), ST_Point(3, 4)]),
    (3, ST_GeomFromText('POLYGON((0 0, 1 0, 1 1, 0 0))'), []::GEOMETRY[]),
    (4, ST_GeomFromText('POINT Z (1 2 3)'), NULL),
    (5, NULL, NULL)");

$want_hex = [
    1 => '0101000000000000000000F87F000000000000F87F',
    2 => '01020000000200000000000000000000000000000000000000000000000000F03F000000000000F03F',
    3 => '01030000000100000004000000'
        . '00000000000000000000000000000000'
        . '000000000000F03F0000000000000000'
        . '000000000000F03F000000000000F03F'
        . '00000000000000000000000000000000',
];
$want_wkt = [
    1 => 'POINT EMPTY',
    2 => 'LINESTRING (0 0, 1 1)',
    3 => 'POLYGON ((0 0, 1 0, 1 1, 0 0))',
    4 => 'POINT Z (1 2 3)',
];

$rt = $db->prepare('SELECT ST_AsText(ST_GeomFromHEXWKB(?)) AS w');
$rows = $db->query('SELECT id, geom, geoms, CAST(geoms AS VARCHAR) AS geoms_want FROM g ORDER BY id')
    ->fetchAll(PDO::FETCH_ASSOC);
foreach ($rows as $row) {
    $id = (int) $row['id'];
    if ($id === 5) {
        var_dump($row['geom']);
        var_dump($row['geoms']);
        continue;
    }
    if ($id === 4) {
        // 3D WKB axis encoding is GEOS-version dependent; lock the stable
        // facts instead: 29 bytes of little-endian hex that round-trips.
        echo 'id=4 hex_len=', strlen($row['geom']), ' hex_prefix=', substr($row['geom'], 0, 2), "\n";
    } else {
        echo "id=$id hex=", ($row['geom'] === $want_hex[$id] ? 'ok' : 'MISMATCH ' . $row['geom']), "\n";
    }
    $rt->execute([$row['geom']]);
    $wkt = $rt->fetchColumn();
    echo "id=$id wkt=", ($wkt === $want_wkt[$id] ? $wkt : 'MISMATCH ' . $wkt), "\n";
    // Nested GEOMETRY has no C-API value constructor, so the driver encodes
    // elements with the same uppercase hex as scalar fetches (engine CAST
    // renders WKT instead, so an explicit expectation -- not a differential
    // one -- locks the driver contract here).
    if ($row['geoms'] === null) {
        echo "id=$id geoms=null\n";
    } elseif ($id === 2) {
        $want_list = '[0101000000000000000000F03F0000000000000040, 010100000000000000000008400000000000001040]';
        echo 'id=2 geoms=', ($row['geoms'] === $want_list ? 'hex_list_ok' : 'MISMATCH ' . $row['geoms']), "\n";
        $has12 = str_contains($row['geoms'], '0101000000000000000000F03F0000000000000040');
        $has34 = str_contains($row['geoms'], '010100000000000000000008400000000000001040');
        echo 'id=2 elems=', ($has12 && $has34 ? 'ok' : 'MISMATCH ' . $row['geoms']), "\n";
    } else {
        echo "id=$id geoms=", ($row['geoms'] === $row['geoms_want'] ? 'match_engine' : 'MISMATCH ' . $row['geoms'] . ' vs ' . $row['geoms_want']), "\n";
    }
}

// STRUCT and MAP containers substitute VARCHAR for GEOMETRY the same way:
// elements render as uppercase hex, keyed exactly like the engine shapes.
$s = $db->query("SELECT {'g': ST_Point(1, 2)} AS s")->fetchColumn();
echo 'struct=', ($s === "{'g': 0101000000000000000000F03F0000000000000040}" ? 'hex_struct_ok' : 'MISMATCH ' . $s), "\n";
$m = $db->query("SELECT MAP {'a': ST_Point(3, 4)} AS m")->fetchColumn();
echo 'map=', ($m === '{a=010100000000000000000008400000000000001040}' ? 'hex_map_ok' : 'MISMATCH ' . $m), "\n";
$a = $db->query("SELECT [ST_Point(1, 2), ST_Point(3, 4)]::GEOMETRY[2] AS a")->fetchColumn();
echo 'array=', ($a === '[0101000000000000000000F03F0000000000000040, 010100000000000000000008400000000000001040]' ? 'hex_array_ok' : 'MISMATCH ' . $a), "\n";
$u = $db->query("SELECT union_value(g := ST_Point(1, 2)) AS u")->fetchColumn();
echo 'union=', ($u === '0101000000000000000000F03F0000000000000040' ? 'hex_union_ok' : 'MISMATCH ' . $u), "\n";

// One unbuffered fetch of the same scalar path.
$stream = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_UNBUFFERED => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$stream->exec('INSTALL spatial; LOAD spatial;');
var_dump($stream->query('SELECT ST_Point(1, 2)')->fetchColumn());
?>
--EXPECT--
string(42) "0101000000000000000000F03F0000000000000040"
string(11) "POINT (1 2)"
id=1 hex=ok
id=1 wkt=POINT EMPTY
id=1 geoms=null
id=2 hex=ok
id=2 wkt=LINESTRING (0 0, 1 1)
id=2 geoms=hex_list_ok
id=2 elems=ok
id=3 hex=ok
id=3 wkt=POLYGON ((0 0, 1 0, 1 1, 0 0))
id=3 geoms=match_engine
id=4 hex_len=58 hex_prefix=01
id=4 wkt=POINT Z (1 2 3)
id=4 geoms=null
NULL
NULL
struct=hex_struct_ok
map=hex_map_ok
array=hex_array_ok
union=hex_union_ok
string(42) "0101000000000000000000F03F0000000000000040"
--CLEAN--
<?php
?>
