--TEST--
pdo_duckdb: a failed missing-file ATTACH cannot block unrelated database opens after its path becomes uninspectable
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('symlink')) die('skip symbolic links unavailable');
$path = __DIR__ . '/153_pending_symlink_probe';
$available = @symlink(basename($path), $path);
@unlink($path);
if (!$available) die('skip filesystem cannot create symbolic links');
?>
--FILE--
<?php
$root = __DIR__ . '/153_failed_pending_work';
mkdir($root);
mkdir($root . '/pending');
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$owner = new PDO('duckdb::memory:', null, null, $options);
try {
    $owner->exec('ATTACH ' . $owner->quote($root . '/pending/missing.duckdb') . ' AS missing (READ_ONLY)');
    echo "missing_attach=BAD allowed\n";
} catch (PDOException $e) {
    if ($e->errorInfo[0] !== 'HY000' || !str_contains($e->getMessage(), 'does not exist')) {
        throw new Exception('Unexpected ATTACH failure: ' . $e->getMessage());
    }
    echo "missing_attach=refused\n";
}
if (!rmdir($root . '/pending') || !symlink('pending', $root . '/pending')) {
    throw new Exception('Could not make the failed path uninspectable');
}
$unrelated = new PDO('duckdb:' . $root . '/unrelated.duckdb', null, null, $options);
echo 'unrelated=', $unrelated->query('SELECT 42')->fetchColumn(), "\n";
echo 'original_owner=', $owner->query('SELECT 43')->fetchColumn(), "\n";
$unrelated = $owner = null;
?>
--CLEAN--
<?php
$root = __DIR__ . '/153_failed_pending_work';
@unlink($root . '/pending/missing.duckdb');
@unlink($root . '/pending/missing.duckdb.wal');
@unlink($root . '/pending');
@rmdir($root . '/pending');
@unlink($root . '/unrelated.duckdb');
@unlink($root . '/unrelated.duckdb.wal');
@rmdir($root);
@unlink(__DIR__ . '/153_pending_symlink_probe');
?>
--EXPECT--
missing_attach=refused
unrelated=42
original_owner=43
