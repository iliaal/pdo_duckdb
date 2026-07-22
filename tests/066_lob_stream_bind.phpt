--TEST--
pdo_duckdb: PDO::PARAM_LOB stream resource binds binary data
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (b BLOB)');

$bin = "\x00\x01\x02binary\xff";
$stream = fopen('php://memory', 'r+b');
fwrite($stream, $bin);
rewind($stream);

$stmt = $db->prepare('INSERT INTO t VALUES (?)');
$stmt->bindValue(1, $stream, PDO::PARAM_LOB);
$stmt->execute();
fclose($stream);

$got = $db->query('SELECT b FROM t')->fetchColumn();
var_dump($got === $bin);

// Non-stream resource must fail cleanly without poisoning the next execute.
$proc = @proc_open('true', [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
if (!is_resource($proc)) {
    echo "skip_proc\n";
    exit(0);
}
foreach ($pipes as $p) {
    if (is_resource($p)) {
        fclose($p);
    }
}

$stmt2 = $db->prepare('INSERT INTO t VALUES (?)');
try {
    $stmt2->bindValue(1, $proc, PDO::PARAM_LOB);
    $stmt2->execute();
    echo "BAD: process resource accepted as LOB\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'Expected a stream resource') || str_contains($e->getMessage(), 'HY105')
        ? "bind_rejected\n" : ("bind_other=" . $e->getMessage() . "\n");
}
proc_close($proc);

// Next execute with a valid value must still work (binds_cleared hygiene).
$stmt3 = $db->prepare('INSERT INTO t VALUES (?)');
$stmt3->bindValue(1, 'ok', PDO::PARAM_LOB);
$stmt3->execute();
echo "rebind_ok\n";
?>
--EXPECT--
bool(true)
bind_rejected
rebind_ok
