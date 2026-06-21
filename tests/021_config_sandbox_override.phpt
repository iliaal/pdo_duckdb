--TEST--
pdo_duckdb: open_basedir sandbox overrides user-supplied enable_external_access
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
// Even when the caller explicitly tries to re-enable external access (via both
// the DSN and the config array), the open_basedir sandbox is applied last and
// wins: external file access stays disabled.
$db = new PDO(
    'duckdb::memory:;enable_external_access=true',
    null, null,
    [PDO::DUCKDB_ATTR_CONFIG => ['enable_external_access' => 'true']]
);
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$v = $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn();
var_dump($v === 'false');

try {
    $db->query("SELECT * FROM read_csv('/etc/hostname')")->fetchAll();
    echo "NOT BLOCKED (BUG)\n";
} catch (PDOException $e) {
    echo "external read blocked\n";
}
?>
--EXPECT--
bool(true)
external read blocked
--CLEAN--
<?php
?>
