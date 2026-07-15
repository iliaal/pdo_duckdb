--TEST--
pdo_duckdb: executing EXPLAIN wrappers preserve transaction state across all execution paths
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER)');

$cases = [
    'begin' => 'EXPLAIN ANALYZE BEGIN',
    'start' => 'EXPLAIN (ANALYZE) START TRANSACTION',
    'commit' => 'EXPLAIN (FORMAT JSON, ANALYZE FALSE) COMMIT',
    'end' => 'EXPLAIN ("ANALYZE") END',
    'rollback' => 'EXPLAIN (ANALYSE) ROLLBACK',
    'abort' => 'EXPLAIN (FORMAT JSON, "ANALYZE") ABORT',
];

foreach ($cases as $label => $sql) {
    $closing = in_array($label, ['commit', 'end', 'rollback', 'abort'], true);
    if ($closing) {
        $db->beginTransaction();
    }
    $db->exec($sql);
    echo "$label state=", $db->inTransaction() ? 'active' : 'idle', "\n";
    if (!$closing) {
        $db->rollBack();
    }
}

$stmt = $db->prepare('EXPLAIN ANALYZE BEGIN');
$stmt->execute();
echo 'prepared state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();

$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
$stmt = $db->prepare('EXPLAIN (ANALYZE) BEGIN');
$stmt->execute();
echo 'unbuffered state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$stmt->closeCursor();
$db->rollBack();
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, false);

$db->exec('INSERT INTO t VALUES (1); EXPLAIN ANALYZE BEGIN');
echo 'opening batch state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->rollBack();
echo 'opening batch rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

$db->exec('BEGIN; INSERT INTO t VALUES (2); EXPLAIN ANALYZE COMMIT; INSERT INTO t VALUES (3)');
echo 'closing batch state=', $db->inTransaction() ? 'active' : 'idle', "\n";
echo 'closing batch rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

try {
    $db->exec('EXPLAIN ANALYZE BEGIN; INSERT INTO missing_table VALUES (4)');
    echo "failing batch accepted (BUG)\n";
} catch (PDOException $e) {
    echo 'failure state=', $db->inTransaction() ? 'active' : 'idle', "\n";
}
$db->rollBack();

$db->exec('EXPLAIN BEGIN');
echo 'plain explain state=', $db->inTransaction() ? 'active' : 'idle', "\n";
$db->exec('EXPLAIN ANALYZE SELECT CASE WHEN true THEN 1 END');
echo 'analyzed select state=', $db->inTransaction() ? 'active' : 'idle', "\n";

$options = [
    PDO::ATTR_PERSISTENT => 'pdo-duckdb-060',
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
];
$persistent = new PDO('duckdb::memory:', null, null, $options);
$persistent->exec('CREATE TABLE persistent_t (id INTEGER)');
$profile = $persistent->query('EXPLAIN ANALYZE BEGIN');
$profile->fetchAll();
$persistent->exec('INSERT INTO persistent_t VALUES (9)');
unset($profile, $persistent);

$persistent = new PDO('duckdb::memory:', null, null, $options);
echo 'persistent state=', $persistent->inTransaction() ? 'active' : 'idle', "\n";
echo 'persistent rows=', $persistent->query('SELECT count(*) FROM persistent_t')->fetchColumn(), "\n";
?>
--EXPECT--
begin state=active
start state=active
commit state=idle
end state=idle
rollback state=idle
abort state=idle
prepared state=active
unbuffered state=active
opening batch state=active
opening batch rows=1
closing batch state=idle
closing batch rows=3
failure state=active
plain explain state=idle
analyzed select state=idle
persistent state=idle
persistent rows=0
