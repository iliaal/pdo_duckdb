--TEST--
pdo_duckdb: tightening open_basedir on a handle with quack loaded refuses all SQL and stops its listeners
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
try {
    $db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
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
    $db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('LOAD httpfs; LOAD quack');
    return $db;
}

function attempt(PDO $db, string $label, callable $fn): void {
    try {
        $fn($db);
        echo "$label: ALLOWED\n";
    } catch (PDOException $e) {
        $refused = str_contains($e->getMessage(), 'not contained by it (quack)');
        echo "$label: ", $refused ? 'refused' : 'other error: ' . strtok($e->getMessage(), "\n"), "\n";
    }
}

$port = free_port();
$uri = "quack:127.0.0.1:$port";
$opts = "token='tttt', disable_ssl=true";

// Positive control without open_basedir: $server listens, and the client
// surfaces reach it.
$server = open_with_quack();
$server->exec('CREATE TABLE srv AS SELECT 42 AS v');
$server->query("CALL quack_serve('$uri', $opts)")->fetchAll();
echo 'listening: ', listening($port) ? 'yes' : 'no', "\n";
var_dump((int) $server->query("SELECT * FROM quack_query('$uri', 'SELECT 7', $opts)")->fetchColumn());
$server->exec("ATTACH '$uri' AS q (TOKEN 'tttt', DISABLE_SSL true)");
var_dump((int) $server->query('SELECT v FROM q.srv')->fetchColumn());

// $client is CONNECTed to $server on DuckDB 2.0 (1.5.6 has no CONNECT).
$client = open_with_quack();
$client->exec("ATTACH '$uri' AS q (TOKEN 'tttt', DISABLE_SSL true)");
try {
    $client->exec('CONNECT q');
} catch (PDOException $e) {
}

// $db only loads quack before the tightening.
$db = open_with_quack();
$prepared = $db->prepare('SELECT 1');

ini_set('open_basedir', __DIR__);

// Refusing the client must not run its containment SQL on the remote side.
attempt($client, 'connected client', fn($db) => $db->query('SELECT 1')->fetchAll());
echo 'listener after refusing the client: ', listening($port) ? 'yes' : 'STOPPED', "\n";

$port2 = free_port();
attempt($db, 'select', fn($db) => $db->query('SELECT 1')->fetchAll());
attempt($db, 'quack_query', fn($db) => $db->query("SELECT * FROM quack_query('$uri', 'SELECT 7', $opts)")->fetchAll());
attempt($db, 'attach', fn($db) => $db->exec("ATTACH '$uri' AS q (TOKEN 'tttt', DISABLE_SSL true)"));
attempt($db, 'attached table', fn($db) => $db->query('SELECT v FROM q.srv')->fetchAll());
attempt($db, 'connect', fn($db) => $db->exec('CONNECT q'));
attempt($db, 'quack_serve', fn($db) => $db->query("CALL quack_serve('quack:127.0.0.1:$port2', $opts)")->fetchAll());
attempt($db, 'prepared before tightening', fn($db) => $prepared->execute());
attempt($db, 'beginTransaction', fn($db) => $db->beginTransaction());
attempt($db, 'appender', fn($db) => $db->duckdbAppender('srv'));
attempt($db, 'tableNames', fn($db) => $db->duckdbTableNames('SELECT * FROM srv'));
var_dump($db->errorInfo()[1]);
echo 'listener requested after tightening: ', listening($port2) ? 'LISTENING' : 'absent', "\n";

// The server handle is escalated on its next use: refused, and the listener it
// opened before the tightening is stopped.
attempt($server, 'server handle', fn($db) => $db->query('SELECT v FROM q.srv')->fetchAll());
echo 'listener started before tightening: ', listening($port) ? 'STILL LISTENING' : 'stopped', "\n";

// A handle opened under open_basedir is sandboxed at open: quack cannot load.
$fresh = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
try {
    $fresh->exec('LOAD quack');
    echo "fresh LOAD: ALLOWED\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled through configuration'));
}
var_dump((int) $fresh->query('SELECT 42')->fetchColumn());
?>
--EXPECT--
listening: yes
int(7)
int(42)
connected client: refused
listener after refusing the client: yes
select: refused
quack_query: refused
attach: refused
attached table: refused
connect: refused
quack_serve: refused
prepared before tightening: refused
beginTransaction: refused
appender: refused
tableNames: refused
int(4)
listener requested after tightening: absent
server handle: refused
listener started before tightening: stopped
bool(true)
int(42)
