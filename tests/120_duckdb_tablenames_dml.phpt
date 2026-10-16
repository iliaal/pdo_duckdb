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
// DuckDB 1.5.3 and 1.5.4 cannot bind MERGE for table-name extraction, so it throws there.
$lib = ltrim($db->query("SELECT library_version FROM pragma_version()")->fetchColumn(), 'v');
$merge_supported = version_compare($lib, '1.5.5', '>=');
try {
    $merge = $db->duckdbTableNames("MERGE INTO x USING t ON x.a = t.a WHEN MATCHED THEN UPDATE SET a = t.a");
    echo $merge_supported && $merge === [] ? "merge: expected\n" : "merge: unexpected " . var_export($merge, true) . "\n";
} catch (PDOException $e) {
    echo !$merge_supported && $e->getMessage() === "PDO::duckdbTableNames(): could not parse the query"
        ? "merge: expected\n" : "merge: unexpected " . $e->getMessage() . "\n";
}

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
merge: expected
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
