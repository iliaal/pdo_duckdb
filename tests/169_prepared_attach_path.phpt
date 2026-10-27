--TEST--
pdo_duckdb: ATTACH path bindings have no prepare-time effects and preserve quoted paths
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$path = __DIR__ . '/attach_' . bin2hex(random_bytes(8)) . "'quoted.duckdb";
try {
    $stmt = $db->prepare('; /* :ignored ? */ ; ATTACH ? AS bound_file');
    if (file_exists($path) || $db->query("SELECT count(*) FROM duckdb_databases() WHERE database_name='bound_file'")->fetchColumn() !== 0) {
        throw new RuntimeException('prepare attached or created the database');
    }
    $stmt->execute([$path]);
    if (!file_exists($path)) {
        throw new RuntimeException('execute did not create the bound database path');
    }
    $db->exec('CREATE TABLE bound_file.t(v VARCHAR)');
    $payload = "attached\0'" . bin2hex(random_bytes(8));
    $db->prepare('INSERT INTO bound_file.t VALUES (?)')->execute([$payload]);
    $db->exec('DETACH bound_file');
    $reopened = new PDO('duckdb:' . $path);
    if ($reopened->query('SELECT v FROM t')->fetchAll(PDO::FETCH_COLUMN) !== [$payload]) {
        throw new RuntimeException('Reopened bound path did not contain the written bytes');
    }
    unset($reopened);
    echo "prepare is inert; execute attaches the exact path and persists data\n";
} finally {
    unset($stmt, $db);
    @unlink($path);
    @unlink($path . '.wal');
}
?>
--EXPECT--
prepare is inert; execute attaches the exact path and persists data
