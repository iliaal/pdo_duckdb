--TEST--
pdo_duckdb: successful ops clear sticky driver errorInfo payload
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

// Force a connection-level error.
$ok = $db->exec('SELECT * FROM definitely_missing_table_xyz');
echo "failed_exec=" . var_export($ok, true) . "\n";
$info = $db->errorInfo();
echo "after_error_sqlstate={$info[0]}\n";
echo "after_error_has_msg=" . (isset($info[2]) && $info[2] !== '' ? "yes" : "no") . "\n";

// Successful work must not keep the old driver code/message under 00000.
$ok = $db->exec('SELECT 1');
echo "ok_exec=" . var_export($ok === 0 || $ok === false ? $ok : (int)$ok, true) . "\n";
$info = $db->errorInfo();
// PDO reports SQLSTATE 00000 on success and pads errorInfo to 3 slots with
// nulls when the driver has no sticky code/message.
echo "after_ok_sqlstate={$info[0]}\n";
echo "after_ok_driver=" . var_export($info[1] ?? null, true) . "\n";
echo "after_ok_msg=" . var_export($info[2] ?? null, true) . "\n";
?>
--EXPECTF--
failed_exec=false
after_error_sqlstate=HY000
after_error_has_msg=yes
ok_exec=%d
after_ok_sqlstate=00000
after_ok_driver=NULL
after_ok_msg=NULL
