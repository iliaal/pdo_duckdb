--TEST--
pdo_duckdb: missing and excess PDO bindings never reuse stale values or session variables
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
memory_limit=80M
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t(v INTEGER); SET VARIABLE x=7; SET VARIABLE y=92');
$variable = $v2 ? '$x' : "getvariable('x')";
$stmt = $db->prepare("INSERT INTO t SELECT $variable + CAST(:x AS INTEGER) + CAST(:y AS INTEGER)");
$stmt->execute(['x' => 2, 'y' => 3]);
$stmt->execute();
$expected = [12, 12];
foreach ([[], ['x' => 4], ['y' => 5], ['x' => 2, 'y' => 3, 'extra' => 99]] as $values) {
    try {
        $stmt->execute($values);
        throw new RuntimeException('Incomplete or excess bind set executed');
    } catch (PDOException $e) {
        if (!str_contains(strtolower($e->getMessage()), 'param')
            && !str_contains($e->getMessage(), 'bound variables')) {
            throw $e;
        }
    }
    if ($stmt->rowCount() !== 0 || $db->query('SELECT v FROM t ORDER BY v')->fetchAll(PDO::FETCH_COLUMN) !== $expected) {
        throw new RuntimeException('Failed binding changed data or retained an affected-row count');
    }
}
$stmt->execute(['x' => 5, 'y' => 6]);
$expected[] = 18;
if ($stmt->rowCount() !== 1 || $db->query('SELECT v FROM t ORDER BY v')->fetchAll(PDO::FETCH_COLUMN) !== $expected) {
    throw new RuntimeException('Valid execution failed after binding errors');
}
$repeated = $db->prepare("SELECT $variable + CAST(:x AS INTEGER) + CAST(:x AS INTEGER)");
$x = 2;
$repeated->bindParam('x', $x, PDO::PARAM_INT);
$repeated->execute();
if ($repeated->fetchAll(PDO::FETCH_COLUMN) !== [11]) {
    throw new RuntimeException('Repeated named binding required duplicate values');
}
$x = 9;
$repeated->execute();
if ($repeated->fetchAll(PDO::FETCH_COLUMN) !== [25]
    || $db->query("SELECT getvariable('x'), getvariable('y')")->fetch(PDO::FETCH_NUM) !== [7, 92]) {
    throw new RuntimeException('Reference binding or session variables changed incorrectly');
}
echo "missing/excess refusal, no stale writes, repeated names, and references verified\n";
$stream = tmpfile();
$block = str_repeat('a', 1024 * 1024);
for ($i = 0; $i < 32; $i++) {
    fwrite($stream, $block);
}
unset($block);
rewind($stream);
$lob = $db->prepare('SELECT octet_length(?)');
$lob->bindValue(1, $stream, PDO::PARAM_LOB);
for ($i = 0; $i < 2; $i++) {
    $lob->execute();
    if ($lob->fetchAll(PDO::FETCH_COLUMN) !== [32 * 1024 * 1024]) {
        throw new RuntimeException('Native LOB binding or replay changed');
    }
}
fclose($stream);
echo "native LOB replay fits the PHP memory limit\n";
?>
--EXPECT--
missing/excess refusal, no stale writes, repeated names, and references verified
native LOB replay fits the PHP memory limit
