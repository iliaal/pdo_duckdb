--TEST--
pdo_duckdb: cycle-collected appender and PDO flush in either free order without touching a freed handle
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// The cycle collector calls free_obj on garbage in buffer order, so the PDO
// object (and its connection) can be freed before the appender that still
// references it. Rows must be flushed by the appender's destructor while the
// connection is open, and an open transaction must still be rolled back.
$dir = __DIR__ . '/101_work';
@mkdir($dir);
$path = $dir . '/gc.duckdb';

function make_cycle(string $path, bool $appender_low_handle, bool $in_txn): void
{
    $placeholder = $appender_low_handle ? new stdClass : null;
    $db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $db->exec('CREATE OR REPLACE TABLE t (i INTEGER)');
    // Frees the lower object handle so the appender reuses it.
    $placeholder = null;
    if ($in_txn) {
        $db->beginTransaction();
    }
    $ap = $db->duckdbAppender('t');
    $ap->appendRow(1);
    $ap->appendRow(2);
    $holder = new stdClass;
    $holder->self = $holder;
    $holder->db = $db;
    $holder->ap = $ap;
}

foreach ([false, true] as $low) {
    foreach ([false, true] as $txn) {
        make_cycle($path, $low, $txn);
        $collected = gc_collect_cycles();
        $db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
        printf("low=%d txn=%d collected=%s rows=%d\n", $low, $txn,
            $collected > 0 ? 'yes' : 'no', $db->query('SELECT count(*) FROM t')->fetchColumn());
        $db = null;
    }
}
echo "done\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/101_work';
foreach (glob($dir . '/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
low=0 txn=0 collected=yes rows=2
low=0 txn=1 collected=yes rows=0
low=1 txn=0 collected=yes rows=2
low=1 txn=1 collected=yes rows=0
done
