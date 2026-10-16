--TEST--
pdo_duckdb: execute() array keys at the zend_long limit fail as out-of-range binds
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $db->prepare('SELECT ?::VARCHAR AS a');

foreach (['PHP_INT_MAX' => PHP_INT_MAX, 'PHP_INT_MAX - 1' => PHP_INT_MAX - 1, '-1' => -1, '0' => 0] as $label => $key) {
    try {
        $st->execute([$key => 'x']);
        echo "key $label: ", json_encode($st->fetchAll(PDO::FETCH_NUM)), "\n";
    } catch (PDOException $e) {
        echo "key $label: ", $e->getMessage(), "\n";
    }
}

try {
    $st->bindValue(PHP_INT_MAX, 'y');
    $st->execute();
    echo "bindValue PHP_INT_MAX: bound\n";
} catch (PDOException $e) {
    echo "bindValue PHP_INT_MAX: ", $e->getMessage(), "\n";
}

$st = $db->prepare('SELECT ?::VARCHAR AS a, ?::INTEGER AS b');
$st->execute(['v', 7]);
var_dump($st->fetch(PDO::FETCH_ASSOC));
?>
--EXPECT--
key PHP_INT_MAX: SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
key PHP_INT_MAX - 1: SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
key -1: SQLSTATE[HY000]: General error: 1 Cannot bind a parameter without a position
key 0: [["x"]]
bindValue PHP_INT_MAX: SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
array(2) {
  ["a"]=>
  string(1) "v"
  ["b"]=>
  int(7)
}
