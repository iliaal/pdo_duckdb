--TEST--
pdo_duckdb: statement errorInfo clears on re-execute; failed prepare clears handle error
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

// Statement errorInfo is per-execute: a failure records ...
$st = $db->prepare('SELECT CAST(? AS INTEGER)');
var_dump($st->execute(['not-an-integer']));
$info = $st->errorInfo();
echo "fail_sqlstate={$info[0]}\n";
echo 'fail_msg=' . (isset($info[2]) && str_contains($info[2], 'not-an-integer') ? "yes\n" : "no\n");

// ... and the next successful re-execute clears it back to exactly 00000.
var_dump($st->execute([42]));
echo 'value=', $st->fetchColumn(), "\n";
$info = $st->errorInfo();
echo "ok_sqlstate={$info[0]}\n";
echo 'entries=' . count($info) . "\n";
echo 'clean=', (!isset($info[1]) && !isset($info[2]) ? "yes\n" : "no\n");

// A failed prepare records on the handle ...
var_dump($db->prepare('SELECT FROM )('));
$info = $db->errorInfo();
echo "prep_fail_sqlstate={$info[0]}\n";

// ... cleared by the next successful prepare: PDO resets the handle SQLSTATE
// at prepare entry and the driver drops its payload on success.
$ok = $db->prepare('SELECT 1');
var_dump($ok !== false);
$ok->execute();
echo 'value=', $ok->fetchColumn(), "\n";
$info = $db->errorInfo();
echo "prep_ok_sqlstate={$info[0]}\n";
echo 'entries=' . count($info) . "\n";
echo 'clean=', (!isset($info[1]) && !isset($info[2]) ? "yes\n" : "no\n");
?>
--EXPECT--
bool(false)
fail_sqlstate=HY000
fail_msg=yes
bool(true)
value=42
ok_sqlstate=00000
entries=3
clean=yes
bool(false)
prep_fail_sqlstate=42000
bool(true)
value=1
prep_ok_sqlstate=00000
entries=3
clean=yes
