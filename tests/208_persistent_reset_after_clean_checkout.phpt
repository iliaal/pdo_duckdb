--TEST--
pdo_duckdb: a persistent checkout that skipped the instance-wide resets still resets settings a later request changed
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$read = function (PDO $db): string {
    return implode('|', array_map(
        fn ($name) => (string)$db->query("SELECT current_setting('$name')")->fetchColumn(),
        ['http_proxy', 'http_proxy_username', 'http_proxy_password']));
};

$db = new PDO('duckdb::memory:', null, null, $opts);
unset($db);
/* The first checkout resets and records the settings. */
$db = new PDO('duckdb::memory:', null, null, $opts);
echo 'first=', $read($db), "\n";
unset($db);
/* Nothing changed, so this checkout can skip the resets. */
$db = new PDO('duckdb::memory:', null, null, $opts);
echo 'clean=', $read($db), "\n";
$db->exec("SET http_proxy='http://127.0.0.1:1'");
$db->exec("SET http_proxy_username='u'");
$db->exec("SET http_proxy_password='p'");
echo 'set=', $read($db), "\n";
unset($db);
$db = new PDO('duckdb::memory:', null, null, $opts);
echo 'checkout=', $read($db), "\n";
?>
--EXPECT--
first=||
clean=||
set=http://127.0.0.1:1|u|p
checkout=||
