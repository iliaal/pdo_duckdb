--TEST--
pdo_duckdb: EXEC_PRE conversions read a copy and re-read the bound variable
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// A Stringable bound by value: PDO core converts at bind time.
final class JustString
{
    public function __toString(): string
    {
        return 'ok';
    }
}
$stmt = $db->prepare('SELECT ?');
$stmt->bindValue(1, new JustString(), PDO::PARAM_STR);
echo 'bindValue=', $stmt->execute() ? $stmt->fetchColumn() : 'fail', "\n";

// bindParam by reference: the object lands in the variable after the bind, so
// the driver's EXEC_PRE hook is what converts it -- once per execute, against
// the current value of the variable.
final class Counter
{
    public int $n = 0;

    public function __toString(): string
    {
        return 'v' . (++$this->n);
    }
}
$ref = null;
$stmt = $db->prepare('SELECT ?');
$stmt->bindParam(1, $ref, PDO::PARAM_STR);
$ref = new Counter();
$stmt->execute();
echo 'exec1=', $stmt->fetchColumn(), "\n";
$stmt->execute();
echo 'exec2=', $stmt->fetchColumn(), "\n";
echo 'calls=', $ref->n, "\n";

// A PARAM_LOB stream is drained by the driver on every execute; the stream is
// rewound afterwards so a re-execute binds the same bytes, not an empty string.
$db->exec('CREATE TABLE t (b BLOB)');
$stream = fopen('php://memory', 'r+b');
fwrite($stream, "\x00lob\xff");
rewind($stream);
$stmt = $db->prepare('INSERT INTO t VALUES (?)');
$stmt->bindValue(1, $stream, PDO::PARAM_LOB);
$stmt->execute();
$stmt->execute();
fclose($stream);
var_dump($db->query('SELECT count(*), count(DISTINCT b) FROM t')->fetch(PDO::FETCH_NUM));

// Re-entering execute() from __toString is a PDO core hazard (heap-use-after-free
// in really_register_bound_param / dispatch_param_event), not a driver one; see
// beads aph-1nr and aph-30q. Not exercised here.
?>
--EXPECT--
bindValue=ok
exec1=v1
exec2=v2
calls=2
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(1)
}
