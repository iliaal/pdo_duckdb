--TEST--
pdo_duckdb: END and ABORT keep multi-statement and persistent transaction state synchronized
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
foreach ([['END', 1], ['ABORT', 0]] as [$verb, $expectedRows]) {
    $db = new PDO('duckdb::memory:');
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $db->exec('CREATE TABLE t (id INTEGER)');
    $db->exec('BEGIN');
    $db->exec("INSERT INTO t VALUES (1); /* transaction alias */ $verb; SELECT 1");

    echo "$verb state=", $db->inTransaction() ? 'active' : 'idle', "\n";
    echo "$verb rows=", $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
    var_dump($db->beginTransaction());
    var_dump($db->rollBack());
}

$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec(<<<'SQL'
SELECT 'END; ABORT', $$EXPLAIN ANALYZE BEGIN; COMMIT$$, E'quote\'; END';
/* BEGIN; ROLLBACK */ BEGIN
SQL);
echo 'quoted state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();
$db->exec('SELECT CASE WHEN true THEN 1 END AS begin;');
echo 'case state=', $db->inTransaction() ? 'active' : 'idle', "\n";

$unicodeSpaces = [
    'nbsp' => "\xC2\xA0",
    'en-quad' => "\xE2\x80\x80",
    'hair-space' => "\xE2\x80\x8A",
    'narrow-nbsp' => "\xE2\x80\xAF",
    'math-space' => "\xE2\x81\x9F",
    'ideographic-space' => "\xE3\x80\x80",
    'bom' => "\xEF\xBB\xBF",
];
foreach ($unicodeSpaces as $name => $space) {
    $db->exec($space . 'EXPLAIN ANALYZE BEGIN');
    echo "$name state=", $db->inTransaction() ? 'active' : 'idle', "\n";
    $db->rollBack();
}
$db->exec("EXPLAIN\xC2\xA0ANALYZE\xE2\x80\x83BEGIN");
echo 'unicode separators state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();

$db->exec('CREATE TABLE unicode_tx (id INTEGER)');
$db->exec("\xEF\xBB\xBFBEGIN; INSERT INTO unicode_tx VALUES (1)");
echo 'unicode batch state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();
echo 'unicode batch rows=', $db->query('SELECT count(*) FROM unicode_tx')->fetchColumn(), "\n";

$options = [
    PDO::ATTR_PERSISTENT => 'pdo-duckdb-059',
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
];
$db = new PDO('duckdb::memory:', null, null, $options);
$db->exec('CREATE TABLE persistent_t (id INTEGER)');
$db->exec('BEGIN');
$db->exec('END; SELECT 1');
$db->exec('BEGIN');
$db->exec('INSERT INTO persistent_t VALUES (42)');
unset($db);

$db = new PDO('duckdb::memory:', null, null, $options);
echo 'persistent state=', $db->inTransaction() ? 'active' : 'idle', "\n";
echo 'persistent rows=', $db->query('SELECT count(*) FROM persistent_t')->fetchColumn(), "\n";
?>
--EXPECT--
END state=idle
END rows=1
bool(true)
bool(true)
ABORT state=idle
ABORT rows=0
bool(true)
bool(true)
quoted state=active
case state=idle
nbsp state=active
en-quad state=active
hair-space state=active
narrow-nbsp state=active
math-space state=active
ideographic-space state=active
bom state=active
unicode separators state=active
unicode batch state=active
unicode batch rows=0
persistent state=idle
persistent rows=0
