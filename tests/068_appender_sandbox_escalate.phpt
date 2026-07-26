--TEST--
pdo_duckdb: Appender appendRow/flush escalate open_basedir sandbox
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Appender create latches the sandbox; live methods must escalate if basedir
// is tightened after create (otherwise sticky external access stays on).
// On 8.4+ use PDO::connect() so duckdbAppender() is not a deprecated base-PDO method.
$db = PHP_VERSION_ID >= 80400
	? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION])
	: new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (id INTEGER, name VARCHAR)');
$app = $db->duckdbAppender('t');

ini_set('open_basedir', __DIR__);

// Live methods re-apply escalate; row still appends (in-memory table).
$app->appendRow(1, 'ok');
$app->flush();
$app->close();

$row = $db->query('SELECT id, name FROM t')->fetch(PDO::FETCH_NUM);
echo "row={$row[0]},{$row[1]}\n";

// External file access must be denied after appender-path escalate.
try {
    $db->query("SELECT * FROM read_csv('/etc/hostname')");
    echo "external=BAD\n";
} catch (PDOException $e) {
    echo "external=blocked\n";
}
?>
--EXPECT--
row=1,ok
external=blocked
