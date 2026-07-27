--TEST--
pdo_duckdb: re-narrowing open_basedir mid-fetch fails closed at the next chunk pull
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// The fetcher re-latches the sandbox when it pulls a chunk, not on every row
// (rows served out of an already-materialized chunk touch no filesystem). A
// basedir re-narrow must still fail closed once the current chunk runs out.
$base = __DIR__;
$public = $base . '/076_public_' . getmypid();
@mkdir($public, 0700, true);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
ini_set('open_basedir', $base);

// DuckDB emits 2048-row chunks, so this spans several.
$stmt = $db->query('SELECT i FROM range(20000) tbl(i)');

$seen = 0;
while ($seen < 10 && $stmt->fetch(PDO::FETCH_NUM) !== false) {
    $seen++;
}
echo "before_renarrow=$seen\n";

ini_set('open_basedir', $public);

try {
    while ($stmt->fetch(PDO::FETCH_NUM) !== false) {
        $seen++;
        if ($seen > 20000) {
            break;
        }
    }
    echo "renarrow=BAD_completed_at_$seen\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'sandbox') ? "renarrow=fail_closed\n"
        : ('renarrow=' . $e->getMessage() . "\n");
    // It must fail no later than the end of the chunk the re-narrow landed in.
    echo 'failed_within_one_chunk=', ($seen <= 2048 ? 'yes' : "no($seen)"), "\n";
}
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/076_public_*') ?: [] as $d) {
    @rmdir($d);
}
?>
--EXPECT--
before_renarrow=10
renarrow=fail_closed
failed_within_one_chunk=yes
