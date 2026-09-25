--TEST--
pdo_duckdb: throwing PDO string conversion clears stale SELECT and INSERT state
--EXTENSIONS--
pdo
pdo_duckdb
--XFAIL--
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$thrower = new class {
    public function __toString(): string
    {
        throw new RuntimeException('no string');
    }
};

$select = $db->prepare('SELECT ? AS v');
$select->execute([123]);
echo "select_first=", $select->fetchColumn(), "\n";
try {
    $select->execute([$thrower]);
} catch (RuntimeException $e) {
    echo "select_exception=yes\n";
}
echo "select_stale=", var_export($select->fetchColumn(), true), "\n";
echo "select_columns=", $select->columnCount(), "\n";

$db->exec('CREATE TABLE t (v INTEGER)');
$insert = $db->prepare('INSERT INTO t VALUES (?)');
$insert->execute([7]);
echo "insert_first=", $insert->rowCount(), "\n";
try {
    $insert->execute([$thrower]);
} catch (RuntimeException $e) {
    echo "insert_exception=yes\n";
}
$insert->fetch();
echo "insert_rows_after_failure=", $insert->rowCount(), "\n";
echo "table_rows=", $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
select_first=123
select_exception=yes
select_stale=false
select_columns=0
insert_first=1
insert_exception=yes
insert_rows_after_failure=0
table_rows=1
