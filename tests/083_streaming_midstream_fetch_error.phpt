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
$db->exec('SET threads=1');
$db->exec("SET streaming_buffer_size='64KB'");

$sql = "SELECT CAST(CASE WHEN i=1000000 THEN ? || i::VARCHAR ELSE i::VARCHAR END AS INTEGER) FROM range(1000001) t(i)";
$stmt = $db->prepare($sql);
$stmt->execute(['not-an-integer']);
$seen = 0;
try {
    while (($value = $stmt->fetchColumn()) !== false) {
        if ($value !== $seen++ || $seen > 1000000) {
            throw new RuntimeException('Streaming result changed a valid row before its conversion error');
        }
    }
    echo "BAD: invalid value fetched\n";
} catch (PDOException $e) {
    if ($seen < 2048) {
        throw new RuntimeException('Streaming error surfaced before a complete valid chunk was returned', 0, $e);
    }
    echo str_contains($e->getMessage(), 'Conversion Error') ? "fetch_raised\n" : ('other=' . $e->getMessage() . "\n");
}

// Silent-mode fetch errors carry the streaming/fetch driver code.
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->prepare($sql);
$stmt->execute(['not-an-integer']);
$seen = 0;
while (($value = $stmt->fetchColumn()) !== false) {
    if ($value !== $seen++ || $seen > 1000000) {
        throw new RuntimeException('Silent streaming result changed a valid row before its conversion error');
    }
}
if ($seen < 2048) {
    throw new RuntimeException('Silent streaming error surfaced before a complete valid chunk was returned');
}
var_dump($value);
$info = $stmt->errorInfo();
echo "fetch_sqlstate={$info[0]}\n";
echo 'fetch_driver=' . var_export($info[1], true) . "\n";

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

echo $db->query('SELECT 7')->fetchColumn(), "\n";

$db->exec('CREATE TABLE t (i INTEGER)');
$ins = $db->prepare('INSERT INTO t VALUES (1), (2), (3)');
$ins->execute();
echo 'rowcount=', $ins->rowCount(), "\n";
echo 'count=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
fetch_raised
bool(false)
fetch_sqlstate=HY000
fetch_driver=5
7
rowcount=3
count=3
