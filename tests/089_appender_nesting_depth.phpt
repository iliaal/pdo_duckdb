--TEST--
pdo_duckdb: Appender rejects rows nested deeper than 128 levels
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// A 129-deep INTEGER list column: 129 levels must fail, exactly 128 must land.
$db->exec('CREATE TABLE t (c INTEGER' . str_repeat('[]', 129) . ')');
$app = $db->duckdbAppender('t');

$v129 = 1;
for ($i = 0; $i < 129; $i++) {
    $v129 = [$v129];
}
try {
    $app->appendRow($v129);
    echo "BAD: 129-deep row accepted\n";
} catch (PDOException $e) {
    echo $e->getMessage() === 'Pdo\Duckdb\Appender::appendRow(): maximum nesting depth (128) exceeded'
        ? "depth_rejected\n" : ('other=' . $e->getMessage() . "\n");
}

// The rejected row is refused before any native append, so the appender is
// still usable. A 128-deep value needs its own 128-deep column: against the
// 129-deep column it would be a shape mismatch, not a depth pass.
$db->exec('CREATE TABLE t128 (c INTEGER' . str_repeat('[]', 128) . ')');
$app128 = $db->duckdbAppender('t128');
$v128 = 7;
for ($i = 0; $i < 128; $i++) {
    $v128 = [$v128];
}
$app128->appendRow($v128);
$app128->flush();
echo 'rows=', $db->query('SELECT count(*) FROM t128')->fetchColumn(), "\n";
?>
--EXPECT--
depth_rejected
rows=1
