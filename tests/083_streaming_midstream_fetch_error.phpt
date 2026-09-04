--TEST--
pdo_duckdb: unbuffered failures surface at fetch, handle stays reusable
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

// VARIANT has no safe C-API extractor: the query itself executes (the value
// computes fine), the error surfaces per-cell at fetch, not at execute.
$stmt = $db->query('SELECT CAST(42 AS VARIANT) AS v');
try {
    $stmt->fetchColumn();
    echo "BAD: VARIANT fetched\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'VARIANT') ? "fetch_raised\n" : ('other=' . $e->getMessage() . "\n");
}

// Silent-mode fetch errors carry the streaming/fetch driver code.
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->query('SELECT CAST(42 AS VARIANT)');
var_dump($stmt->fetchColumn());
$info = $stmt->errorInfo();
echo "fetch_sqlstate={$info[0]}\n";
echo 'fetch_driver=' . var_export($info[1], true) . "\n";

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// The handle is reusable right after the mid-stream failure ...
echo $db->query('SELECT 7')->fetchColumn(), "\n";

// ... and streaming DML still reports affected rows.
$db->exec('CREATE TABLE t (i INTEGER)');
$ins = $db->prepare('INSERT INTO t VALUES (1), (2), (3)');
$ins->execute();
echo 'rowcount=', $ins->rowCount(), "\n";
echo 'count=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
fetch_raised
NULL
fetch_sqlstate=HY000
fetch_driver=5
7
rowcount=3
count=3
