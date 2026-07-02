--TEST--
pdo_duckdb: Appender rejects scalar integer overflow before poisoning native state
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER, c TINYINT, u UTINYINT)');

$ap = $db->duckdbAppender('t');
$ap->appendRow(0, 0, 0);

try {
    $ap->appendRow(1, 300, 1);
    echo "BAD: tinyint overflow accepted\n";
} catch (ValueError $e) {
    echo "tinyint overflow rejected\n";
}

try {
    $ap->appendRow(2, 1, -1);
    echo "BAD: utinyint negative accepted\n";
} catch (ValueError $e) {
    echo "utinyint negative rejected\n";
}

$ap->appendRow(3, 127, 255);
$ap->flush();

foreach ($db->query('SELECT id, c, u FROM t ORDER BY id') as $row) {
    echo "{$row['id']}|{$row['c']}|{$row['u']}\n";
}
?>
--EXPECT--
tinyint overflow rejected
utinyint negative rejected
0|0|0
3|127|255
