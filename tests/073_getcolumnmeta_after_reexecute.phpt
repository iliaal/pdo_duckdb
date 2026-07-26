--TEST--
pdo_duckdb: getColumnMeta works after re-execute without fetch
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $db->prepare('SELECT 1 AS a, 2 AS b');
$st->execute();
$m1 = $st->getColumnMeta(0);
echo 'first=', $m1['name'], "\n";

// Re-execute frees PDO columns via set_column_count(0); must rebuild for meta.
$st->execute();
$m2 = $st->getColumnMeta(0);
$m3 = $st->getColumnMeta(1);
echo 'second=', $m2['name'], ',', $m3['name'], "\n";
echo 'columnCount=', $st->columnCount(), "\n";

// closeCursor zeros columns; next execute must restore meta again.
$st->closeCursor();
echo 'after_close=', $st->columnCount(), "\n";
$st->execute();
$m4 = $st->getColumnMeta(0);
echo 'after_reopen=', $m4['name'], "\n";
?>
--EXPECT--
first=a
second=a,b
columnCount=2
after_close=0
after_reopen=a
