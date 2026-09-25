--TEST--
pdo_duckdb: later core string conversion failure clears immediate metadata and row count
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

$select = $db->prepare('SELECT ?, ? AS pair');
$select->execute([1, 2]);
echo "select_first=", $select->fetchColumn(), "\n";
try {
    $select->execute([3, $thrower]);
} catch (RuntimeException $e) {
    echo "select_exception=yes\n";
}
echo "select_columns=", $select->columnCount(), "\n";
echo "select_meta=", $select->getColumnMeta(0) === false ? 'false' : 'unexpected', "\n";
echo "select_rows=", $select->rowCount(), "\n";

$db->exec('CREATE TABLE t (a INTEGER, b INTEGER)');
$insert = $db->prepare('INSERT INTO t VALUES (?, ?)');
$insert->execute([4, 5]);
echo "insert_first=", $insert->rowCount(), "\n";
try {
    $insert->execute([6, $thrower]);
} catch (RuntimeException $e) {
    echo "insert_exception=yes\n";
}
echo "insert_rows=", $insert->rowCount(), "\n";
echo "table_rows=", $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
select_first=1
select_exception=yes
select_columns=0
select_meta=false
select_rows=0
insert_first=1
insert_exception=yes
insert_rows=0
table_rows=1
