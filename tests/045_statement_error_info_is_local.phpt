--TEST--
pdo_duckdb: statement errorInfo keeps each statement's native message
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

$a = $db->prepare('SELECT CAST(? AS INTEGER)');
$b = $db->prepare('SELECT CAST(? AS INTEGER)');

$a->execute(['aaa']);
$aFirst = $a->errorInfo()[2];

$b->execute(['bbb']);
$aAfterB = $a->errorInfo()[2];
$bError = $b->errorInfo()[2];

echo 'A first has aaa: ', str_contains($aFirst, 'aaa') ? 'yes' : 'no', "\n";
echo 'A after B still has aaa: ', str_contains($aAfterB, 'aaa') ? 'yes' : 'no', "\n";
echo 'A after B hides bbb: ', str_contains($aAfterB, 'bbb') ? 'no' : 'yes', "\n";
echo 'B has bbb: ', str_contains($bError, 'bbb') ? 'yes' : 'no', "\n";
?>
--EXPECT--
A first has aaa: yes
A after B still has aaa: yes
A after B hides bbb: yes
B has bbb: yes
