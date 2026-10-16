--TEST--
pdo_duckdb: DuckDB 2.0 CONNECT and quack client/server surfaces are denied on a handle sandboxed at open
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) {
    die('skip needs a build against the DuckDB 2.0 headers');
}
try {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('INSTALL httpfs; INSTALL quack; LOAD httpfs; LOAD quack');
} catch (PDOException $e) {
    die('skip quack extension not available: ' . strtok($e->getMessage(), "\n"));
}
?>
--FILE--
<?php
function free_port(): int {
    $s = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    $port = (int) substr(strrchr(stream_socket_get_name($s, false), ':'), 1);
    fclose($s);
    return $port;
}

function listening(int $port): bool {
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if ($c) {
        fclose($c);
        return true;
    }
    return false;
}

function open_with_quack(): PDO {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('LOAD httpfs; LOAD quack');
    return $db;
}

function attempt(string $label, callable $fn, string $expect): void {
    try {
        $fn();
        echo "$label: ALLOWED\n";
    } catch (PDOException $e) {
        echo "$label: ", str_contains($e->getMessage(), $expect) ? 'denied' : 'other error: ' . strtok($e->getMessage(), "\n"), "\n";
    }
}

$port = free_port();
$uri = "quack:127.0.0.1:$port";
$auth = "(TOKEN 'tttt', DISABLE_SSL true)";

$server = open_with_quack();
$server->query("CALL quack_serve('$uri', token='tttt', disable_ssl=true)")->fetchAll();
$tables = fn(string $name) => (int) $server->query("SELECT count(*) FROM duckdb_tables() WHERE table_name = '$name'")->fetchColumn();

// Positive controls without open_basedir: both CONNECT forms retarget the
// session to the server, and DISCONNECT returns it.
$client = open_with_quack();
$client->exec("CONNECT '$uri' $auth");
$client->exec('CREATE TABLE via_uri AS SELECT 1 AS v');
$client->exec('DISCONNECT');
$client->exec("ATTACH '$uri' AS q $auth");
$client->exec('CONNECT q');
$client->exec('CREATE TABLE via_name AS SELECT 1 AS v');
$client->exec('DISCONNECT');
$client->exec('CREATE TABLE local_only AS SELECT 1 AS v');
var_dump($tables('via_uri'), $tables('via_name'), $tables('local_only'));
$client = null;

ini_set('open_basedir', __DIR__);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$port2 = free_port();
attempt('LOAD quack', fn() => $db->exec('LOAD quack'), 'Loading external extensions is disabled');
attempt('CONNECT uri', fn() => $db->exec("CONNECT '$uri' $auth"), 'Loading external extensions is disabled');
attempt('ATTACH quack', fn() => $db->exec("ATTACH '$uri' AS q $auth"), 'Loading external extensions is disabled');
attempt('CONNECT name', fn() => $db->exec('CONNECT q'), 'is not attached');
$db->exec("ATTACH ':memory:' AS m");
attempt('CONNECT local catalog', fn() => $db->exec('CONNECT m'), 'does not support CONNECT');
attempt('quack_query', fn() => $db->query("SELECT * FROM quack_query('$uri', 'SELECT 7', token='tttt', disable_ssl=true)")->fetchAll(), 'not in the catalog');
attempt('quack_serve', fn() => $db->query("CALL quack_serve('quack:127.0.0.1:$port2', token='tttt', disable_ssl=true)")->fetchAll(), 'not in the catalog');
var_dump((int) $db->query('SELECT 42')->fetchColumn());
echo 'listener requested under the sandbox: ', listening($port2) ? 'LISTENING' : 'absent', "\n";

// The server handle is escalated on its next use, which refuses it and stops
// its listener.
attempt('server handle', fn() => $server->query('SELECT 1')->fetchAll(), 'not contained by it (quack)');
echo 'listener after the server handle is refused: ', listening($port) ? 'STILL LISTENING' : 'stopped', "\n";
?>
--EXPECT--
int(1)
int(1)
int(0)
LOAD quack: denied
CONNECT uri: denied
ATTACH quack: denied
CONNECT name: denied
CONNECT local catalog: denied
quack_query: denied
quack_serve: denied
int(42)
listener requested under the sandbox: absent
server handle: denied
listener after the server handle is refused: stopped
