--TEST--
pdo_duckdb: tightening open_basedir refuses a handle with any extension outside the contained set, not one with httpfs or spatial
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
try {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('INSTALL httpfs; INSTALL inet; INSTALL spatial; LOAD httpfs; LOAD inet; LOAD spatial');
} catch (PDOException $e) {
    die('skip httpfs/inet/spatial extensions not available: ' . strtok($e->getMessage(), "\n"));
}
?>
--FILE--
<?php
function open_with(string $ext): PDO {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec("LOAD $ext");
    return $db;
}

$httpfs = open_with('httpfs');
$spatial = open_with('spatial');
$inet = open_with('inet');
var_dump($inet->query("SELECT host('192.168.1.5/24'::INET)")->fetchColumn());

ini_set('open_basedir', __DIR__);

// httpfs reads go through DuckDB's file system layer, which the profile denies.
var_dump((int) $httpfs->query('SELECT 42')->fetchColumn());
try {
    $httpfs->query("SELECT * FROM read_csv('http://127.0.0.1:9/x.csv')")->fetchAll();
    echo "httpfs read: ALLOWED\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}

// spatial's GDAL opens its own HTTP connections; spatial denies its VSI and
// driver prefixes itself once enable_external_access is off. Nothing may reach
// the local listener.
$listener = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
$port = (int) substr(strrchr(stream_socket_get_name($listener, false), ':'), 1);
var_dump((int) $spatial->query('SELECT 42')->fetchColumn());
try {
    $spatial->query("SELECT * FROM ST_Read('/vsicurl/http://127.0.0.1:$port/x.geojson')")->fetchAll();
    echo "spatial /vsicurl/: ALLOWED\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'External access is disabled'));
}
var_dump(@stream_socket_accept($listener, 0) === false);
fclose($listener);

// inet does no network I/O, but it is not in the vetted set: unknown
// extensions are refused rather than trusted.
for ($i = 0; $i < 2; $i++) {
    try {
        $inet->query('SELECT 1')->fetchAll();
        echo "inet handle: ALLOWED\n";
    } catch (PDOException $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
string(11) "192.168.1.5"
int(42)
bool(true)
int(42)
bool(true)
bool(true)
SQLSTATE[HY000]: General error: 4 Unable to apply the open_basedir sandbox profile to DuckDB: loaded DuckDB extensions are not contained by it (inet); the connection refuses further SQL
SQLSTATE[HY000]: General error: 4 Unable to apply the open_basedir sandbox profile to DuckDB: loaded DuckDB extensions are not contained by it (inet); the connection refuses further SQL
