--TEST--
pdo_duckdb: missing named parameter clears the previous result
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->prepare('SELECT :value AS v');
$stmt->execute(['value' => 123]);
echo "first=", $stmt->fetchColumn(), "\n";
echo "failed=", $stmt->execute(['missing' => 'x']) ? 'no' : 'yes', "\n";
echo "stale=", var_export($stmt->fetchColumn(), true), "\n";
echo "columns=", $stmt->columnCount(), "\n";
?>
--EXPECT--
first=123
failed=yes
stale=false
columns=0
