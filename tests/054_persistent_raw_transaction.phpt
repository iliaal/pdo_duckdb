--TEST--
pdo_duckdb: raw transaction statements stay synchronized with PDO state
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$options = [
    PDO::ATTR_PERSISTENT => 'pdo-duckdb-054',
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
];

$db = new PDO('duckdb::memory:', null, null, $options);
$db->exec('CREATE TABLE tx (id INTEGER)');
$db->exec('BEGIN');
$db->exec('INSERT INTO tx VALUES (1)');
var_dump($db->inTransaction());
unset($db);

$db = new PDO('duckdb::memory:', null, null, $options);
echo 'after reuse=', $db->query('SELECT count(*) FROM tx')->fetchColumn(), "\n";
var_dump($db->inTransaction());

$begin = $db->prepare('BEGIN');
$begin->execute();
var_dump($db->inTransaction());
$rollback = $db->prepare('ROLLBACK');
$rollback->execute();
var_dump($db->inTransaction());

$db->exec('BEGIN; INSERT INTO tx VALUES (2)');
var_dump($db->inTransaction());
$db->rollBack();
echo 'after multi=', $db->query('SELECT count(*) FROM tx')->fetchColumn(), "\n";

try {
    $db->exec('BEGIN; INSERT INTO missing_table VALUES (3)');
    echo "failing multi accepted (BUG)\n";
} catch (PDOException $e) {
    echo "failing multi rejected\n";
}
var_dump($db->inTransaction());
$db->rollBack();

$db->exec('BEGIN');
$db->exec('INSERT INTO tx VALUES (4)');
$same = new PDO('duckdb::memory:', null, null, $options);
echo 'live wrapper=', $same->query('SELECT count(*) FROM tx')->fetchColumn(), "\n";
var_dump($same->inTransaction());
$db->rollBack();
unset($same);

var_dump($db->beginTransaction());
var_dump($db->rollBack());
?>
--EXPECT--
bool(true)
after reuse=0
bool(false)
bool(true)
bool(false)
bool(true)
after multi=0
failing multi rejected
bool(true)
live wrapper=1
bool(true)
bool(true)
bool(true)
