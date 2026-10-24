--TEST--
pdo_duckdb: an unreadable extension .info refuses a runtime open_basedir escalation without waiting out the install-race retry
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// The listing retries only errors that name a temporary install file. A
// corrupt or empty .info under the installed name is permanent and must
// refuse at once, not after the half-second retry budget.
$work = __DIR__ . '/149_work_' . getmypid();
mkdir($work);
$dummy = "$work/pdo149dummy.duckdb_extension";
file_put_contents($dummy, str_repeat("\0", 4096));

$cases = ['corrupt' => 'garbage', 'empty' => ''];
$handles = [];
foreach ($cases as $case => $contents) {
    $ext = "$work/$case";
    mkdir($ext);
    $installer = new PDO('duckdb::memory:;allow_unsigned_extensions=true', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $installer->exec('SET extension_directory=' . $installer->quote($ext));
    $installer->exec('SET allow_extensions_metadata_mismatch=true');
    $installer->exec('INSTALL ' . $installer->quote($dummy));
    $installer = null;
    $info = glob("$ext/*/*/pdo149dummy.duckdb_extension.info");
    if (count($info) !== 1) {
        echo "$case: no installed .info\n";
        continue;
    }
    file_put_contents($info[0], $contents);

    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $db->exec('SET extension_directory=' . $db->quote($ext));
    $handles[$case] = $db;
}

ini_set('open_basedir', $work);

foreach ($handles as $case => $db) {
    $start = hrtime(true);
    try {
        $db->exec('SELECT 1');
        echo "$case: ALLOWED\n";
    } catch (PDOException $e) {
        $ms = (hrtime(true) - $start) / 1e6;
        echo "$case: ", str_contains($e->getMessage(), 'could not be listed') ? 'refused' : 'other error: ' . $e->getMessage(),
            $ms < 200 ? '' : sprintf(' after %.0f ms', $ms), "\n";
    }
}
?>
--CLEAN--
<?php
function pdo149_rmtree(string $path): void {
    if (is_dir($path) && !is_link($path)) {
        foreach (scandir($path) ?: [] as $entry) {
            if ($entry !== '.' && $entry !== '..') {
                pdo149_rmtree("$path/$entry");
            }
        }
        @rmdir($path);
    } else {
        @unlink($path);
    }
}
foreach (glob(__DIR__ . '/149_work_*') ?: [] as $dir) {
    pdo149_rmtree($dir);
}
?>
--EXPECT--
corrupt: refused
empty: refused
