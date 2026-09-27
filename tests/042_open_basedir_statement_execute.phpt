--TEST--
pdo_duckdb: open_basedir tightened after prepare still sandboxes statement execute
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$base = __DIR__ . '/042_open_basedir_statement_execute';
mkdir($base);
mkdir($base . '/allowed');
$file = $base . '/outside.csv';
$marker = 'pdo_duckdb_042_' . bin2hex(random_bytes(8));
file_put_contents($file, $marker . "\n");

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// Prepare before open_basedir is tightened. Explicit columns avoid testing CSV
// schema sniffing during prepare; the file access should be blocked at execute.
$stmt = $db->prepare('SELECT * FROM read_csv(' . $db->quote($file)
    . ", columns={'line': 'VARCHAR'}, header=false)");
$stmt->execute();
var_dump($stmt->fetchColumn() === $marker);
$stmt->closeCursor();

ini_set('open_basedir', $base . '/allowed');

try {
    $stmt->execute();
    echo "BYPASS via prepared execute\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'disabled by configuration'));
}

var_dump((int) $db->query('SELECT 42')->fetchColumn());
?>
--EXPECT--
bool(true)
bool(true)
int(42)
--CLEAN--
<?php
$base = __DIR__ . '/042_open_basedir_statement_execute';
if (is_file($base . '/outside.csv')) {
    unlink($base . '/outside.csv');
}
if (is_dir($base . '/allowed')) {
    rmdir($base . '/allowed');
}
if (is_dir($base)) {
    rmdir($base);
}
?>
