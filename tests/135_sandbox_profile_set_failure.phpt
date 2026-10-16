--TEST--
pdo_duckdb: a sandbox profile SET that fails at escalation refuses SQL instead of running unsandboxed
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
// A configuration locked before the tightening makes the first profile SET fail.
$db->exec('SET lock_configuration=true');
var_dump((int) $db->query('SELECT 1')->fetchColumn());

ini_set('open_basedir', __DIR__);

foreach (['SELECT 1', "SELECT count(*) FROM read_text('" . __FILE__ . "')", 'SELECT 2'] as $sql) {
    try {
        $db->query($sql)->fetchAll();
        echo "$sql: ALLOWED\n";
    } catch (PDOException $e) {
        echo "$sql: ", str_contains($e->getMessage(), 'Unable to apply the open_basedir sandbox profile') ? 'refused' : 'other error', "\n";
    }
}
try {
    $db->exec('SELECT 3');
    echo "exec: ALLOWED\n";
} catch (PDOException $e) {
    echo 'exec: ', str_contains($e->getMessage(), 'Unable to apply the open_basedir sandbox profile') ? 'refused' : 'other error', "\n";
}
?>
--EXPECTF--
int(1)
SELECT 1: refused
SELECT count(*) FROM read_text('%s'): refused
SELECT 2: refused
exec: refused
