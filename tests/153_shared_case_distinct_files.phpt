--TEST--
pdo_duckdb: existing files differing only by case remain independent and share their own connections
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Darwin' || PHP_OS_FAMILY === 'Windows') {
    die('skip DuckDB case-folding cache cannot keep case-only paths independent; refusal covered separately');
}
$lower = __DIR__ . '/153_case_probe';
$upper = __DIR__ . '/153_CASE_PROBE';
file_put_contents($lower, 'lower');
file_put_contents($upper, 'upper');
$a = stat($lower);
$b = stat($upper);
@unlink($lower);
@unlink($upper);
if ($a['dev'] === $b['dev'] && $a['ino'] === $b['ino']) {
    die('skip filesystem does not support distinct case-only filenames');
}
?>
--FILE--
<?php
$lowerPath = __DIR__ . '/153_case.duckdb';
$upperPath = __DIR__ . '/153_CASE.duckdb';
foreach ([$lowerPath, $upperPath] as $path) {
    @unlink($path);
    @unlink($path . '.wal');
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
foreach ([$lowerPath => 1, $upperPath => 10] as $path => $id) {
    $db = new PDO('duckdb:' . $path . ';threads=1', null, null, $options);
    $db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY)');
    $db->exec('INSERT INTO t VALUES (' . $id . ')');
    $db = null;
}
clearstatcache();
$lowerStat = stat($lowerPath);
$upperStat = stat($upperPath);
echo 'distinct_files=', $lowerStat['dev'] !== $upperStat['dev'] || $lowerStat['ino'] !== $upperStat['ino'] ? 'yes' : 'BAD', "\n";
$rows = static fn(PDO $db): string => implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
$lower = new PDO('duckdb:' . $lowerPath . ';threads=1', null, null, $options);
$upper = new PDO('duckdb:' . $upperPath . ';threads=1', null, null, $options);
$lowerSibling = new PDO('duckdb:' . $lowerPath . ';threads=1', null, null, $options);
$upperSibling = new PDO('duckdb:' . $upperPath . ';threads=1', null, null, $options);
$lower->exec('INSERT INTO t VALUES (2)');
$upper->exec('INSERT INTO t VALUES (20)');
echo 'lower_sibling=', $rows($lowerSibling), "\n";
echo 'upper_sibling=', $rows($upperSibling), "\n";
$lowerSibling->exec('INSERT INTO t VALUES (3)');
$upperSibling->exec('INSERT INTO t VALUES (30)');
echo 'lower=', $rows($lower), "\n";
echo 'upper=', $rows($upper), "\n";
foreach ([$lowerPath, $upperPath] as $path) {
    try {
        $conflict = new PDO('duckdb:' . $path . ';threads=2', null, null, $options);
        echo "config_conflict=BAD\n";
        $conflict = null;
    } catch (PDOException $e) {
        echo 'config_conflict=', $e->getCode() === 2 && $e->errorInfo[0] === '08000' && preg_match('/different.*config/i', $e->getMessage()) ? 'rejected' : 'BAD: ' . $e->getMessage(), "\n";
    }
}
$lower = $lowerSibling = $upper = $upperSibling = null;
foreach ([$lowerPath, $upperPath] as $path) {
    $db = new PDO('duckdb:' . $path, null, null, $options);
    echo 'reopened=', $rows($db), "\n";
    $db = null;
}
?>
--CLEAN--
<?php
foreach (['153_case.duckdb', '153_CASE.duckdb'] as $name) {
    @unlink(__DIR__ . '/' . $name);
    @unlink(__DIR__ . '/' . $name . '.wal');
}
@unlink(__DIR__ . '/153_case_probe');
@unlink(__DIR__ . '/153_CASE_PROBE');
?>
--EXPECT--
distinct_files=yes
lower_sibling=1,2
upper_sibling=10,20
lower=1,2,3
upper=10,20,30
config_conflict=rejected
config_conflict=rejected
reopened=1,2,3
reopened=10,20,30
