--TEST--
pdo_duckdb: a persistent handle left CONNECTed to a remote DuckDB is discarded at checkout
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

function load_quack(PDO $db): void {
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('LOAD httpfs; LOAD quack');
}

$port = free_port();
$uri = "quack:127.0.0.1:$port";

$server = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
load_quack($server);
$server->exec("CALL enable_logging('QueryLog')");
$server->query("CALL quack_serve('$uri', token='tttt', disable_ssl=true)")->fetchAll();

// exec() only: a query() result would keep the persistent handle's refcount
// above one past unset().
$opts = [PDO::ATTR_PERSISTENT => 'pdo-duckdb-132', PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb::memory:', null, null, $opts);
load_quack($a);
$a->exec('CREATE TABLE local_marker AS SELECT 1 AS v');
$a->exec("ATTACH '$uri' AS q (TOKEN 'tttt', DISABLE_SSL true)");
$a->exec('CONNECT q');
// Positive control: the CONNECTed session runs its SQL on the server.
$a->exec('CREATE TABLE made_through_connect AS SELECT 7 AS v');
var_dump((int) $server->query('SELECT v FROM made_through_connect')->fetchColumn());
unset($a);

$b = new PDO('duckdb::memory:', null, null, $opts);
try {
    echo 'checkout handle has local_marker: ',
        $b->query("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'local_marker'")->fetchColumn() ? 'yes' : 'no', "\n";
} catch (PDOException $e) {
    echo 'checkout handle query failed: ', strtok($e->getMessage(), "\n"), "\n";
}
$b->exec('CREATE TABLE made_after_checkout AS SELECT 1 AS v');
echo 'server got the checkout handle\'s table: ',
    $server->query("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'made_after_checkout'")->fetchColumn() ? 'yes' : 'no', "\n";
echo 'server got checkout residue SQL: ',
    $server->query("SELECT count(*) FROM duckdb_logs WHERE type = 'QueryLog' AND (message ILIKE 'RESET %' OR message ILIKE 'PRAGMA disable_profiling%' OR message ILIKE 'SELECT 1 FROM duckdb_extensions()%')")->fetchColumn() ? 'yes' : 'no', "\n";

$server->query("SELECT * FROM quack_stop('$uri')")->fetchAll();
echo 'listener after stop: ', listening($port) ? 'STILL LISTENING' : 'stopped', "\n";
?>
--EXPECT--
int(7)
checkout handle has local_marker: no
server got the checkout handle's table: no
server got checkout residue SQL: no
listener after stop: stopped
