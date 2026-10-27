--TEST--
pdo_duckdb: a detached database's replaced pathname cannot redirect a surviving hard-link alias
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('link')) die('skip hard links unavailable');
$source = __DIR__ . '/153_replace_link_probe';
$target = $source . '.link';
file_put_contents($source, 'probe');
$available = @link($source, $target);
@unlink($target);
@unlink($source);
if (!$available) die('skip filesystem cannot create hard links');
?>
--FILE--
<?php
$root = __DIR__ . '/153_attach_replacement_work';
@mkdir($root);
$original = $root . '/original.duckdb';
$alias = $root . '/alias.duckdb';
$replacement = $root . '/replacement.duckdb';
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
foreach ([$original => 1, $replacement => 99] as $path => $value) {
    $db = new PDO('duckdb:' . $path, null, null, $options);
    $db->exec('CREATE TABLE t(id INTEGER); INSERT INTO t VALUES (' . $value . '); CHECKPOINT');
    $db = null;
}
if (!link($original, $alias)) throw new Exception('Could not create original hard link');
$owner = new PDO('duckdb::memory:', null, null, $options);
$owner->exec('ATTACH ' . $owner->quote($original) . ' AS original');
echo 'before_detach=', $owner->query('SELECT id FROM original.t')->fetchColumn(), "\n";
$owner->exec('DETACH original');
if (!unlink($original) || !copy($replacement, $original)) {
    throw new Exception('Could not replace detached database pathname');
}
clearstatcache();
$old = stat($alias);
$new = stat($original);
echo 'different_identity=', $old['dev'] !== $new['dev'] || $old['ino'] !== $new['ino'] ? 'yes' : 'BAD', "\n";
try {
    $owner->exec('ATTACH ' . $owner->quote($alias) . ' AS surviving');
    echo 'replacement_guard=BAD allowed ', $owner->query('SELECT id FROM surviving.t')->fetchColumn(), "\n";
} catch (PDOException $e) {
    if ($e->errorInfo[0] !== 'HY000' || !preg_match('/replaced|unlinked/i', $e->getMessage())) {
        throw new Exception('Wrong replacement refusal: ' . $e->getMessage());
    }
    echo "replacement_guard=refused\n";
}
$owner = null;
foreach ([$alias, $original] as $path) {
    $db = new PDO('duckdb:' . $path, null, null, $options);
    echo 'after_owner_close=', $db->query('SELECT id FROM t')->fetchColumn(), "\n";
    $db = null;
}
?>
--CLEAN--
<?php
$root = __DIR__ . '/153_attach_replacement_work';
foreach (['original.duckdb', 'alias.duckdb', 'replacement.duckdb'] as $name) {
    @unlink($root . '/' . $name);
    @unlink($root . '/' . $name . '.wal');
}
@rmdir($root);
@unlink(__DIR__ . '/153_replace_link_probe');
@unlink(__DIR__ . '/153_replace_link_probe.link');
?>
--EXPECT--
before_detach=1
different_identity=yes
replacement_guard=refused
after_owner_close=1
after_owner_close=99
