--TEST--
pdo_duckdb: escalate does not allowlist open_basedir for SQL FS; re-narrow fails closed
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// After mid-request escalate, SQL must not read files under the first basedir
// entry (temp_directory must not seed allowed_directories). Re-narrowing
// open_basedir after escalate must fail closed.
$base = __DIR__;
$canary = $base . '/071_canary_' . getmypid() . '.csv';
$public = $base . '/071_public_' . getmypid();
@mkdir($public, 0700, true);
file_put_contents($canary, "col\nsecret\n");

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
ini_set('open_basedir', $base);
$db->exec('SELECT 1'); // escalate

$temp = (string)$db->query("SELECT current_setting('temp_directory')")->fetchColumn();
echo 'temp_empty=', ($temp === '' ? 'yes' : 'no'), "\n";

try {
    $db->query('SELECT * FROM read_csv(' . $db->quote($canary) . ', header=true)');
    echo "in_basedir_sql=BAD\n";
} catch (PDOException $e) {
    echo "in_basedir_sql=blocked\n";
}

// In-memory SQL still works.
echo 'select=', (int)$db->query('SELECT 9')->fetchColumn(), "\n";

// Re-narrow basedir: frozen DuckDB allowlists cannot track PHP policy.
ini_set('open_basedir', $public);
try {
    $db->query('SELECT 1');
    echo "renarrow=BAD\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'sandbox')
        || str_contains($e->getMessage(), 'open_basedir')
        || str_contains($e->getMessage(), 'Unable to apply')
        ? "renarrow=fail_closed\n"
        : ('renarrow=' . $e->getMessage() . "\n");
}
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/071_canary_*.csv') ?: [] as $f) {
    @unlink($f);
}
foreach (glob(__DIR__ . '/071_public_*') ?: [] as $d) {
    @rmdir($d);
}
?>
--EXPECT--
temp_empty=yes
in_basedir_sql=blocked
select=9
renarrow=fail_closed
