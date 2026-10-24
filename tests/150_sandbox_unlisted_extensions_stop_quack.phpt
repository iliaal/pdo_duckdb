--TEST--
pdo_duckdb: when an unreadable .info hides the loaded extensions, a refused escalation still stops quack listeners
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

// An extension directory whose only entry has a corrupt .info: listing it
// fails, so the driver cannot tell that quack is loaded.
$work = __DIR__ . '/150_work_' . getmypid();
$ext = "$work/ext";
mkdir($work);
mkdir($ext);
$dummy = "$work/pdo150dummy.duckdb_extension";
file_put_contents($dummy, str_repeat("\0", 4096));
$installer = new PDO('duckdb::memory:;allow_unsigned_extensions=true', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$installer->exec('SET extension_directory=' . $installer->quote($ext));
$installer->exec('SET allow_extensions_metadata_mismatch=true');
$installer->exec('INSTALL ' . $installer->quote($dummy));
$installer = null;
foreach (glob("$ext/*/*/pdo150dummy.duckdb_extension.info") as $info) {
    file_put_contents($info, 'garbage');
}

$server = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
    $server->exec('SET extension_directory=' . $server->quote($dir));
}
$server->exec('LOAD httpfs; LOAD quack');
$port = free_port();
$server->query("CALL quack_serve('quack:127.0.0.1:$port', token='tttt', disable_ssl=true)")->fetchAll();
echo 'listening: ', listening($port) ? 'yes' : 'no', "\n";
$server->exec('SET extension_directory=' . $server->quote($ext));

// A handle without quack refused the same way must not load it.
$plain = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$plain->exec('SET extension_directory=' . $plain->quote($ext));

ini_set('open_basedir', __DIR__);

foreach (['server' => $server, 'plain' => $plain] as $label => $db) {
    try {
        $db->exec('SELECT 1');
        echo "$label: ALLOWED\n";
    } catch (PDOException $e) {
        echo "$label: ", str_contains($e->getMessage(), 'could not be listed') ? 'refused' : 'other error: ' . $e->getMessage(), "\n";
    }
}
echo 'listener after refusal: ', listening($port) ? 'STILL LISTENING' : 'stopped', "\n";
echo 'quack installed by containment: ', glob("$ext/*/*/quack.duckdb_extension*") ? 'YES' : 'no', "\n";
?>
--CLEAN--
<?php
function pdo150_rmtree(string $path): void {
    if (is_dir($path) && !is_link($path)) {
        foreach (scandir($path) ?: [] as $entry) {
            if ($entry !== '.' && $entry !== '..') {
                pdo150_rmtree("$path/$entry");
            }
        }
        @rmdir($path);
    } else {
        @unlink($path);
    }
}
foreach (glob(__DIR__ . '/150_work_*') ?: [] as $dir) {
    pdo150_rmtree($dir);
}
?>
--EXPECT--
listening: yes
server: refused
plain: refused
listener after refusal: stopped
quack installed by containment: no
