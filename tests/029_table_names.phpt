--TEST--
pdo_duckdb: duckdbTableNames() extracts referenced tables via the parser
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec("CREATE SCHEMA s; CREATE TABLE s.orders(id INT); CREATE TABLE users(id INT);");

// unqualified
var_dump($db->duckdbTableNames("SELECT * FROM users u JOIN s.orders o ON u.id = o.id"));

// qualified: non-default schema prefixed, alias stripped
var_dump($db->duckdbTableNames("SELECT * FROM users u JOIN s.orders o ON u.id = o.id", true));

// CTE name is not a table
var_dump($db->duckdbTableNames("WITH c AS (SELECT 1) SELECT * FROM c, users"));

// a query referencing no table
var_dump($db->duckdbTableNames("SELECT 1 + 1"));

// unparseable query throws
try {
    $db->duckdbTableNames("SELECT FROM )(");
    echo "no throw\n";
} catch (PDOException $e) {
    echo "PDOException: ", $e->getMessage(), "\n";
}

// embedded NUL is rejected
try {
    $db->duckdbTableNames("SELECT * FROM users\0; DROP TABLE users");
    echo "no throw\n";
} catch (ValueError $e) {
    echo "ValueError: ", $e->getMessage(), "\n";
}
?>
--EXPECT--
array(2) {
  [0]=>
  string(6) "orders"
  [1]=>
  string(5) "users"
}
array(2) {
  [0]=>
  string(8) "s.orders"
  [1]=>
  string(5) "users"
}
array(1) {
  [0]=>
  string(5) "users"
}
array(0) {
}
PDOException: PDO::duckdbTableNames(): could not parse the query
ValueError: PDO::duckdbTableNames(): query must not contain a NUL byte
--CLEAN--
<?php
?>
