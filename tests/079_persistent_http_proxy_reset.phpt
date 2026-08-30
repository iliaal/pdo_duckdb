--TEST--
pdo_duckdb: persistent checkout RESETS http_proxy left by the previous request
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$poison = 'http://127.0.0.1:1';

$db = new PDO('duckdb::memory:', null, null, $opts);
$db->exec('SET http_proxy=' . $db->quote($poison));
$before = (string)$db->query("SELECT current_setting('http_proxy')")->fetchColumn();
echo 'set=', $before === $poison ? 'yes' : 'no', "\n";
unset($db);

$db2 = new PDO('duckdb::memory:', null, null, $opts);
$after = (string)$db2->query("SELECT current_setting('http_proxy')")->fetchColumn();
echo 'checkout=', $after === $poison ? 'STALE' : 'reset', "\n";
$httpfs = $db2->query("SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'httpfs'")->fetchColumn();
echo 'httpfs=', ($httpfs === true || $httpfs === 1 || $httpfs === '1' || $httpfs === 't') ? 'LOADED' : 'absent', "\n";
?>
--EXPECT--
set=yes
checkout=reset
httpfs=absent
