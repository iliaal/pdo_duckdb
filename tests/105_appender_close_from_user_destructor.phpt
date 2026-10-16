--TEST--
pdo_duckdb: a user destructor can still close an appender during the shutdown destructor pass
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
?>
--FILE--
<?php
// Shutdown calls destructors in ascending handle order. The importer has a
// higher handle than its appender and a second reference, so its __destruct
// runs after the appender's destructor slot and must find the appender open.
$child = <<<'PHP'
<?php
class Importer
{
    public function __construct(public Pdo\Duckdb\Appender $ap)
    {
    }

    public function __destruct()
    {
        try {
            $this->ap->close();
            echo "closed in destructor\n";
        } catch (Throwable $e) {
            echo get_class($e), ': ', $e->getMessage(), "\n";
        }
    }
}

$db = new PDO('duckdb:' . $argv[1], null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (i INTEGER)');
// new evaluates its arguments after allocating the object.
$ap = $db->duckdbAppender('t');
$imp = new Importer($ap);
$imp->ap->appendRow(1);
$alias = $imp;
if (spl_object_id($imp) <= spl_object_id($imp->ap)) {
    echo "BAD handle order\n";
}
echo "end of script\n";
PHP;

$dir = __DIR__ . '/105_work';
@mkdir($dir);
file_put_contents($dir . '/child.php', $child);
$path = $dir . '/close.duckdb';
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$proc = proc_open($php . ' ' . escapeshellarg($dir . '/child.php') . ' ' . escapeshellarg($path),
    [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
fclose($pipes[0]);
$out = stream_get_contents($pipes[1]);
fclose($pipes[1]);
echo 'child rc=', proc_close($proc), "\n", $out;
$db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
echo 'rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/105_work';
foreach (glob($dir . '/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
child rc=0
end of script
closed in destructor
rows=1
