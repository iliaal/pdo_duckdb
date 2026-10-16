--TEST--
pdo_duckdb: duckdbTableNames() returns [] for UPDATE/DELETE/MERGE instead of throwing
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// On 8.4+ use PDO::connect() -> Pdo\Duckdb subclass, whose methods avoid the
// 8.5 deprecation of base-PDO driver methods. On 8.1-8.3 use new PDO().
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec("CREATE TABLE t(a INTEGER); CREATE TABLE x(a INTEGER);");

// DML statements yield [], per the documented contract.
var_dump($db->duckdbTableNames("UPDATE x SET a = 1"));
var_dump($db->duckdbTableNames("DELETE FROM x"));
var_dump($db->duckdbTableNames("MERGE INTO x USING t ON x.a = t.a WHEN MATCHED THEN UPDATE SET a = t.a"));

// INSERT still extracts the referenced table.
$names = $db->duckdbTableNames("INSERT INTO x SELECT a FROM t");
sort($names);
var_dump($names);

// Genuine parse failures still throw.
try {
    $db->duckdbTableNames("SELECT * FRM x");
    echo "no throw\n";
} catch (PDOException $e) {
    echo "PDOException: ", $e->getMessage(), "\n";
}

// Bind failures (valid syntax, nonexistent table) still throw.
try {
    $db->duckdbTableNames("UPDATE nosuch SET a = 1");
    echo "no throw\n";
} catch (PDOException $e) {
    echo "PDOException: ", $e->getMessage(), "\n";
}

// Statements that prepare but are not UPDATE/DELETE/MERGE keep throwing.
foreach ([
    "SELECT * FROM t WHERE a = ?",
    "SELECT * FROM t WHERE a = $1",
    "EXPLAIN UPDATE x SET a = 1",
] as $sql) {
    try {
        $db->duckdbTableNames($sql);
        echo "no throw: $sql\n";
    } catch (PDOException $e) {
        echo "PDOException: ", $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
array(0) {
}
array(0) {
}
array(0) {
}
array(1) {
  [0]=>
  string(1) "t"
}
PDOException: PDO::duckdbTableNames(): could not parse the query
PDOException: PDO::duckdbTableNames(): could not parse the query
PDOException: PDO::duckdbTableNames(): could not parse the query
PDOException: PDO::duckdbTableNames(): could not parse the query
PDOException: PDO::duckdbTableNames(): could not parse the query
--CLEAN--
<?php
?>
