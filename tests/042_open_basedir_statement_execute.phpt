--TEST--
pdo_duckdb: open_basedir tightened after prepare still sandboxes statement execute
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// Prepare before open_basedir is tightened. Explicit columns avoid testing CSV
// schema sniffing during prepare; the file access should be blocked at execute.
$stmt = $db->prepare("SELECT * FROM read_csv('/etc/hostname', columns={'line': 'VARCHAR'}, header=false)");

ini_set('open_basedir', __DIR__);

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
int(42)
