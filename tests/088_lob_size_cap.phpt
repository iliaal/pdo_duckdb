--TEST--
pdo_duckdb: LOB streams over 64MB are rejected without poisoning the handle
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (b BLOB)');

// Just over the 64MiB cap. Sparse on disk so the fixture itself stays cheap;
// the driver must still refuse the full 67108865 bytes.
$path = sys_get_temp_dir() . '/pdo_duckdb_lobcap_' . getmypid();
$f = fopen($path, 'w+b');
fseek($f, 67108864, SEEK_SET);
fwrite($f, 'x');
fclose($f);

$stream = fopen($path, 'rb');
$st = $db->prepare('INSERT INTO t VALUES (?)');
try {
    $st->bindValue(1, $stream, PDO::PARAM_LOB);
    $st->execute();
    echo "BAD: oversized LOB accepted\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'LOB stream exceeds maximum size of 64MB') ? "cap_rejected\n" : ('other=' . $e->getMessage() . "\n");
}
fclose($stream);
unlink($path);

// The rejection goes through the normal bind-failure path, so rebinding the
// same handle with a valid value still works.
$st->bindValue(1, 'ok', PDO::PARAM_LOB);
$st->execute();
echo 'rebind=', $db->query('SELECT b FROM t')->fetchColumn(), "\n";
?>
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . '/pdo_duckdb_lobcap_*') ?: [] as $f) {
    @unlink($f);
}
?>
--EXPECT--
cap_rejected
rebind=ok
