--TEST--
pdo_duckdb: direct, bound, and batched ATTACH cannot open a competing hard-link instance
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('link')) die('skip hard links unavailable');
$source = __DIR__ . '/153_attach_link_probe';
$target = $source . '.link';
file_put_contents($source, 'probe');
$available = @link($source, $target);
@unlink($target);
@unlink($source);
if (!$available) die('skip filesystem cannot create hard links');
?>
--FILE--
<?php
$root = __DIR__ . '/153_attach_aliases_work';
@mkdir($root);
$original = $root . '/original.duckdb';
$alias = $root . '/alias.duckdb';
$created = $root . '/created.duckdb';
$createdAlias = $root . '/created_alias.duckdb';
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $original, null, null, $options);
$b = new PDO('duckdb::memory:', null, null, $options);
$a->exec('CREATE TABLE t(id INTEGER); INSERT INTO t VALUES (1); CHECKPOINT');
if (!link($original, $alias)) throw new Exception('Could not create database alias');
function refused(string $name, callable $operation, PDO $db): void {
    try {
        $operation();
        echo "$name=BAD allowed\n";
        $db->exec('DETACH conflicting');
    } catch (PDOException $e) {
        if ($e->errorInfo[0] !== 'HY000' || !str_contains($e->getMessage(), 'Unique file handle conflict')) {
            throw new Exception($name . ': wrong refusal: ' . $e->getMessage());
        }
        echo "$name=refused\n";
    }
}
refused('canonical', fn() => $b->exec('ATTACH ' . $b->quote($original) . ' AS conflicting'), $b);
refused('direct_alias', fn() => $b->exec('ATTACH ' . $b->quote($alias) . ' AS conflicting'), $b);
$fileUri = 'file://' . str_replace('\\', '/', $alias);
refused('file_uri_alias', fn() => $b->exec('ATTACH ' . $b->quote($fileUri) . ' AS conflicting'), $b);
refused('bound_alias', function () use ($b, $alias) {
    $statement = $b->prepare('ATTACH ? AS conflicting');
    $statement->execute([$alias]);
}, $b);
refused('batch_alias', fn() => $b->exec('SELECT 42; ATTACH ' . $b->quote($alias) . ' AS conflicting'), $b);
$a->exec('INSERT INTO t VALUES (2)');
$sibling = new PDO('duckdb:' . $original, null, null, $options);
$sibling->exec('INSERT INTO t VALUES (3)');
$rows = static fn(PDO $db, string $table): string => implode(',', $db->query('SELECT id FROM ' . $table . ' ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
echo 'committed=', $rows($a, 't'), "\n";
$b->exec('ATTACH ' . $b->quote($created) . ' AS created');
$b->exec('CREATE TABLE created.t(id INTEGER); INSERT INTO created.t VALUES (10); CHECKPOINT created');
if (!link($created, $createdAlias)) throw new Exception('Could not create attached database alias');
$other = new PDO('duckdb::memory:', null, null, $options);
refused('created_attach_alias', fn() => $other->exec('ATTACH ' . $other->quote($createdAlias) . ' AS conflicting'), $other);
$b->exec('INSERT INTO created.t VALUES (20)');
$a = $b = $sibling = $other = null;
foreach ([$original, $createdAlias] as $path) {
    $db = new PDO('duckdb:' . $path, null, null, $options);
    echo 'reopened=', $rows($db, 't'), "\n";
    $db = null;
}
?>
--CLEAN--
<?php
$root = __DIR__ . '/153_attach_aliases_work';
foreach (['original.duckdb', 'alias.duckdb', 'created.duckdb', 'created_alias.duckdb'] as $name) {
    @unlink($root . '/' . $name);
    @unlink($root . '/' . $name . '.wal');
}
@rmdir($root);
@unlink(__DIR__ . '/153_attach_link_probe');
@unlink(__DIR__ . '/153_attach_link_probe.link');
?>
--EXPECT--
canonical=refused
direct_alias=refused
file_uri_alias=refused
bound_alias=refused
batch_alias=refused
committed=1,2,3
created_attach_alias=refused
reopened=1,2,3
reopened=10,20
