--TEST--
pdo_duckdb: excessive native nesting fails explicitly and leaves the connection usable
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$st = $db->prepare('SELECT CAST(? AS JSON)');
$st->execute([str_repeat('[',130) . '0' . str_repeat(']',130)]);
try {
    $st->fetchColumn();
    throw new Exception('Expected nesting failure');
} catch (PDOException $e) {
    if (!str_contains($e->getMessage(), 'maximum nesting depth')) throw $e;
    echo "depth exception OK\n";
}
if ($db->query('SELECT 7')->fetchColumn() !== 7) throw new Exception('Connection unusable');
echo "connection reuse OK\n";
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$st = $db->prepare('SELECT CAST(? AS JSON)');
$st->execute([str_repeat('[',130) . '0' . str_repeat(']',130)]);
if ($st->fetch() !== false || $st->errorCode() !== 'HY000') throw new Exception('Silent conversion failure');
echo "silent depth error OK\n";
?>
--EXPECT--
depth exception OK
connection reuse OK
silent depth error OK
