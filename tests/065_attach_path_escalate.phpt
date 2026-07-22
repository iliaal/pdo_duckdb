--TEST--
pdo_duckdb: runtime open_basedir escalate detaches out-of-basedir ATTACH paths
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$outside = sys_get_temp_dir() . '/pdo_duckdb_oob_attach_' . getmypid() . '.duckdb';
@unlink($outside);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('ATTACH ' . $db->quote($outside) . ' AS other');
$db->exec('CREATE TABLE other.t(i INTEGER)');
$db->exec('INSERT INTO other.t VALUES (1)');

ini_set('open_basedir', __DIR__);

// Escalate; out-of-basedir attach must be detached so OnSet cannot re-allowlist it.
try {
    $db->query('SELECT * FROM other.t')->fetchAll();
    echo "BAD: other still attached\n";
} catch (PDOException $e) {
    echo "detached\n";
}

// Control: arbitrary path still blocked.
try {
    $db->query("SELECT * FROM read_csv('/etc/hostname', header=false)")->fetchAll();
    echo "BAD: /etc readable\n";
} catch (PDOException $e) {
    echo "etc_blocked\n";
}

var_dump((int)$db->query('SELECT 3')->fetchColumn());
?>
--CLEAN--
<?php
$outside = sys_get_temp_dir() . '/pdo_duckdb_oob_attach_' . getmypid() . '.duckdb';
@unlink($outside);
@unlink($outside . '.wal');
?>
--EXPECT--
detached
etc_blocked
int(3)
