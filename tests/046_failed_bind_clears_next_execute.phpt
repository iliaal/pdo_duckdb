--TEST--
pdo_duckdb: a failed bind round does not leave partial bindings for the next execute
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

$st = $db->prepare('SELECT ? AS v');
$st->execute([123]);
echo 'first value: ', $st->fetchColumn(), "\n";

echo 'extra bind failed: ', $st->execute([456, 999]) ? 'no' : 'yes', "\n";

echo 'empty execute after failed bind failed: ', $st->execute([]) ? 'no' : 'yes', "\n";

$st->execute([789]);
echo 'rebound value: ', $st->fetchColumn(), "\n";
?>
--EXPECT--
first value: 123
extra bind failed: yes
empty execute after failed bind failed: yes
rebound value: 789
