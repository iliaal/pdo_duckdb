--TEST--
pdo_duckdb: driver error codes map connect/syntax/sandbox/general failures distinctly
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
// Connect/open failure: a bogus config key refuses the open outright.
try {
    new PDO('duckdb::memory:;pdo_duckdb_no_such_option_xyz=1');
    echo "BAD: bogus config accepted\n";
} catch (PDOException $e) {
    echo 'connect_code=' . var_export($e->getCode(), true) . "\n";
    echo 'connect_state=' . (str_contains($e->getMessage(), '08000') ? "08000\n" : $e->getMessage() . "\n");
}

$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

// Syntax/prepare failure: unparseable SQL fails in duckdb_prepare.
var_dump($db->prepare('SELECT FROM )('));
$info = $db->errorInfo();
echo "syntax_sqlstate={$info[0]}\n";
echo 'syntax_driver=' . var_export($info[1], true) . "\n";

// Engine-side access denial at prepare: DuckDB itself refuses the read
// because the sandbox disabled external access. Prepare-phase failures map
// to the syntax code; the message carries the denial.
var_dump($db->query("SELECT * FROM read_csv('/etc/hostname')"));
$info = $db->errorInfo();
echo "engine_sqlstate={$info[0]}\n";
echo 'engine_driver=' . var_export($info[1], true) . "\n";
echo 'engine_msg=' . (isset($info[2]) && str_contains($info[2], 'disabled by configuration') ? "yes\n" : "no\n");

// Our own sandbox refusal (re-narrowed basedir fails closed in the preparer)
// keeps the sandbox code and message. open_basedir need not exist as a path
// to be a policy value; narrowing to a subdir is enough to trip the latch.
ini_set('open_basedir', __DIR__ . '/tighten_sub');
var_dump($db->prepare('SELECT 1'));
$info = $db->errorInfo();
echo "sandbox_sqlstate={$info[0]}\n";
echo 'sandbox_driver=' . var_export($info[1], true) . "\n";
echo 'sandbox_msg=' . (isset($info[2]) && str_contains($info[2], 'sandbox') ? "yes\n" : "no\n");

// General runtime failure stays the default code. Last: the handle is
// sandbox-frozen after the re-narrow, so use a fresh unrestricted handle.
$db2 = new PDO('duckdb::memory:');
$db2->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
var_dump($db2->exec('SELECT * FROM definitely_missing_table_xyz'));
$info = $db2->errorInfo();
echo "general_sqlstate={$info[0]}\n";
echo 'general_driver=' . var_export($info[1], true) . "\n";
?>
--EXPECT--
connect_code=2
connect_state=08000
bool(false)
syntax_sqlstate=42000
syntax_driver=3
bool(false)
engine_sqlstate=42000
engine_driver=3
engine_msg=yes
bool(false)
sandbox_sqlstate=HY000
sandbox_driver=4
sandbox_msg=yes
bool(false)
general_sqlstate=HY000
general_driver=1
