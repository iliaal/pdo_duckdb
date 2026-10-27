--TEST--
pdo_duckdb: equivalent relative, symlink, and hard-link paths cannot create competing instances
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip symlink creation requires privileges on Windows');
if (!function_exists('symlink') || !function_exists('link')) die('skip filesystem links unavailable');
?>
--FILE--
<?php
$path = __DIR__ . '/153_shared.duckdb';
$symbolic = __DIR__ . '/153_symbolic.duckdb';
$hard = __DIR__ . '/153_hard.duckdb';
foreach ([$symbolic, $hard, $path, $path . '.wal'] as $file) {
    @unlink($file);
}
$a = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('CREATE TABLE t (id INTEGER PRIMARY KEY)');
$a->exec('INSERT INTO t VALUES (1)');
$a->exec('CHECKPOINT');
var_dump(symlink($path, $symbolic), link($path, $hard));
$cwd = getcwd();
chdir(__DIR__);
$b = new PDO('duckdb:./153_shared.duckdb', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
chdir($cwd);
$c = new PDO('duckdb:' . $symbolic, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('INSERT INTO t VALUES (2)');
$rows = static fn(PDO $db): string => implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
echo 'relative=', $rows($b), "\n";
echo 'symlink=', $rows($c), "\n";
$b->exec('INSERT INTO t VALUES (3)');
$c->exec('INSERT INTO t VALUES (4)');
echo 'canonical=', $rows($a), "\n";

try {
    $d = new PDO('duckdb:' . $hard, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
} catch (PDOException $e) {
    echo 'hardlink_safe=', $e->getCode() === 2 && $e->errorInfo[0] === '08000' && preg_match('/alias|hard.?link|same file/i', $e->getMessage()) ? 'yes' : 'BAD: ' . $e->getMessage(), "\n";
}
if (isset($d)) {
    $a->exec('INSERT INTO t VALUES (5)');
    $d->exec('INSERT INTO t VALUES (6)');
    echo 'hardlink_safe=', $rows($a) === '1,2,3,4,5,6' && $rows($d) === '1,2,3,4,5,6' ? 'yes' : 'BAD', "\n";
    $d->exec('DELETE FROM t WHERE id IN (5, 6)');
    $d = null;
}
$directory = __DIR__ . '/153_alias_case';
@mkdir($directory);
@unlink($directory . '/alias');
if (!link($path, $directory . '/alias')) {
    throw new RuntimeException('Could not create relative hard-link fixture');
}
chdir($directory);
try {
    $relative = new PDO('duckdb:alias', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $a->exec('INSERT INTO t VALUES (7)');
    $relative->exec('INSERT INTO t VALUES (8)');
    echo 'relative_alias_safe=', $rows($a) === '1,2,3,4,7,8' && $rows($relative) === '1,2,3,4,7,8' ? 'yes' : 'BAD', "\n";
    $relative->exec('DELETE FROM t WHERE id IN (7, 8)');
    $relative = null;
} catch (PDOException $e) {
    echo 'relative_alias_safe=', $e->getCode() === 2 && $e->errorInfo[0] === '08000' && preg_match('/alias|hard.?link|same file/i', $e->getMessage()) ? 'yes' : 'BAD: ' . $e->getMessage(), "\n";
} finally {
    chdir($cwd);
}
$a = $b = $c = null;
$reopened = new PDO('duckdb:' . $path);
echo 'reopened=', $rows($reopened), "\n";
$reopened = null;
?>
--CLEAN--
<?php
foreach (['153_shared.duckdb', '153_symbolic.duckdb', '153_hard.duckdb'] as $name) {
    @unlink(__DIR__ . '/' . $name);
    @unlink(__DIR__ . '/' . $name . '.wal');
}
@unlink(__DIR__ . '/153_alias_case/alias');
@unlink(__DIR__ . '/153_alias_case/alias.wal');
@rmdir(__DIR__ . '/153_alias_case');
?>
--EXPECT--
bool(true)
bool(true)
relative=1,2
symlink=1,2
canonical=1,2,3,4
hardlink_safe=yes
relative_alias_safe=yes
reopened=1,2,3,4
