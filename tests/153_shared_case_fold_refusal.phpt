--TEST--
pdo_duckdb: case-folding platforms refuse distinct case-only files while preserving committed data
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Darwin' && PHP_OS_FAMILY !== 'Windows') {
    die('skip requires a platform with the DuckDB case-folding cache');
}
$lower = __DIR__ . '/153_fold_probe';
$upper = __DIR__ . '/153_FOLD_PROBE';
file_put_contents($lower, 'lower');
file_put_contents($upper, 'upper');
$distinct = file_get_contents($lower) === 'lower' && file_get_contents($upper) === 'upper';
@unlink($lower);
@unlink($upper);
if (!$distinct) {
    die('skip filesystem does not support distinct case-only filenames');
}
?>
--FILE--
<?php
$lowerPath = __DIR__ . '/153_fold.duckdb';
$upperPath = __DIR__ . '/153_FOLD.duckdb';
foreach ([$lowerPath, $upperPath] as $path) {
    @unlink($path);
    @unlink($path . '.wal');
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$open = static fn(string $path): PDO => new PDO('duckdb:' . $path . ';threads=1', null, null, $options);
$rows = static fn(PDO $db): string => implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
foreach ([$lowerPath => 1, $upperPath => 10] as $path => $id) {
    $db = $open($path);
    $db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY)');
    $db->exec('INSERT INTO t VALUES (' . $id . ')');
    $db = null;
}
$refused = static function (string $path) use ($open): void {
    try {
        $conflict = $open($path);
        echo "case_conflict=BAD\n";
        $conflict = null;
    } catch (PDOException $e) {
        echo 'case_conflict=', $e->getCode() === 2 && $e->errorInfo[0] === '08000'
            && str_contains($e->getMessage(), 'cannot safely open distinct database paths that differ only by case simultaneously')
            ? 'refused' : 'BAD: ' . $e->getMessage(), "\n";
    }
};
$lower = $open($lowerPath);
$refused($upperPath);
$lowerSibling = $open($lowerPath);
$lowerSibling->exec('INSERT INTO t VALUES (2)');
echo 'lower=', $rows($lower), "\n";
$lower = $lowerSibling = null;
$upper = $open($upperPath);
$refused($lowerPath);
$upperSibling = $open($upperPath);
$upperSibling->exec('INSERT INTO t VALUES (20)');
echo 'upper=', $rows($upper), "\n";
$upper = $upperSibling = null;
foreach ([$lowerPath, $upperPath] as $path) {
    $db = $open($path);
    echo 'reopened=', $rows($db), "\n";
    $db = null;
}
?>
--CLEAN--
<?php
foreach (['153_fold.duckdb', '153_FOLD.duckdb'] as $name) {
    @unlink(__DIR__ . '/' . $name);
    @unlink(__DIR__ . '/' . $name . '.wal');
}
@unlink(__DIR__ . '/153_fold_probe');
@unlink(__DIR__ . '/153_FOLD_PROBE');
?>
--EXPECT--
case_conflict=refused
lower=1,2
case_conflict=refused
upper=10,20
reopened=1,2
reopened=10,20
