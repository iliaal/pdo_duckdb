--TEST--
pdo_duckdb: Appender nested leaves — integer range, BLOB binary, scalar-into-nested
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

function chk(PDO $db, string $col, $val, string $label): void {
    try {
        $db->exec('DROP TABLE IF EXISTS s');
        $db->exec("CREATE TABLE s (c $col)");
        $a = $db->duckdbAppender('s');
        $a->appendRow($val);
        $a->flush();
        echo "$label: OK ", json_encode($db->query('SELECT c FROM s')->fetchColumn()), "\n";
    } catch (\Throwable $e) {
        echo "$label: ", get_class($e), "\n";
    }
}

// Out-of-range nested integers are rejected, not silently wrapped.
chk($db, 'TINYINT[]', [300], 'tinyint overflow');
chk($db, 'UTINYINT[]', [-1], 'utinyint negative');
chk($db, 'TINYINT[]', [-128, 0, 127], 'tinyint in range');
chk($db, 'INTEGER[]', [2147483647], 'int32 max');

// Nested BLOB elements keep binary bytes (not forced through UTF-8 varchar).
$db->exec('DROP TABLE IF EXISTS b');
$db->exec('CREATE TABLE b (c BLOB[])');
$a = $db->duckdbAppender('b');
$a->appendRow(["\x00\x01\xff", "ok"]);
$a->flush();
$got = $db->query('SELECT octet_length(c[1]) AS l FROM b')->fetchColumn();
echo "blob element length: $got\n";

// A non-NULL scalar for a nested column is rejected up front; an earlier-column
// append must NOT have happened, so the appender stays usable.
$db->exec('DROP TABLE IF EXISTS m');
$db->exec('CREATE TABLE m (a INTEGER, b INTEGER[])');
$a = $db->duckdbAppender('m');
try {
    $a->appendRow(1, 'not-an-array');
    echo "scalar-into-nested: no error (BUG)\n";
} catch (\TypeError $e) {
    echo "scalar-into-nested: TypeError\n";
}
$a->appendRow(2, [9, 9]);   // appender not poisoned by the rejected row
$a->flush();
echo "rows after recovery: ", $db->query('SELECT count(*) FROM m')->fetchColumn(), "\n";

// NULL into a nested column is fine (NULL list).
$db->exec('DROP TABLE IF EXISTS n');
$db->exec('CREATE TABLE n (c INTEGER[])');
$a = $db->duckdbAppender('n');
$a->appendRow(null);
$a->flush();
var_dump($db->query('SELECT c FROM n')->fetchColumn());
?>
--EXPECT--
tinyint overflow: ValueError
utinyint negative: ValueError
tinyint in range: OK "[-128, 0, 127]"
int32 max: OK "[2147483647]"
blob element length: 3
scalar-into-nested: TypeError
rows after recovery: 1
NULL
--CLEAN--
<?php
?>
