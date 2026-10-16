--TEST--
pdo_duckdb: refusing a quack handle stops every listener, including one whose URI holds a NUL byte
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

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
    $db->exec('SET extension_directory=' . $db->quote($dir));
}
$db->exec('LOAD httpfs; LOAD quack');

// quack_server_list() has no defined order (the tested builds list newest
// first), so plain listeners go on both sides of the NUL-embedded one.
$ports = ['first plain' => free_port(), 'NUL-embedded' => free_port(), 'last plain' => free_port()];
foreach ($ports as $label => $port) {
    $uri = "'quack:127.0.0.1:$port'" . ($label === 'NUL-embedded' ? " || chr(0) || 'x'" : '');
    $db->query("CALL quack_serve($uri, token='tttt', disable_ssl=true)")->fetchAll();
}
foreach ($ports as $label => $port) {
    echo "$label before: ", listening($port) ? 'listening' : 'absent', "\n";
}

ini_set('open_basedir', __DIR__);

try {
    $db->query('SELECT 1')->fetchAll();
    echo "SELECT: ALLOWED\n";
} catch (PDOException $e) {
    echo 'SELECT: ', str_contains($e->getMessage(), 'not contained by it (quack)') ? 'refused' : 'other error', "\n";
}
foreach ($ports as $label => $port) {
    echo "$label after: ", listening($port) ? 'STILL LISTENING' : 'stopped', "\n";
}
?>
--EXPECT--
first plain before: listening
NUL-embedded before: listening
last plain before: listening
SELECT: refused
first plain after: stopped
NUL-embedded after: stopped
last plain after: stopped
