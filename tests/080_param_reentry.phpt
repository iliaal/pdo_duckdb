--TEST--
pdo_duckdb: EXEC_PRE conversion re-entering execute() must not crash
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

final class ReenterExecute
{
    public $stmt;

    public function __toString(): string
    {
        $this->stmt->execute(['nested', 'x']);
        return 'from-toString';
    }
}

$stmt = $db->prepare('SELECT ? AS a, ? AS b');
$obj = new ReenterExecute();
$obj->stmt = $stmt;
$stmt->bindValue(1, $obj, PDO::PARAM_STR);
$stmt->bindValue(2, 'second', PDO::PARAM_STR);
try {
    $stmt->execute();
    echo "completed\n";
} catch (PDOException $e) {
    echo "threw\n";
}
echo "alive\n";

// Non-reentering Stringable still binds.
final class JustString
{
    public function __toString(): string
    {
        return 'ok';
    }
}
$stmt2 = $db->prepare('SELECT ?');
$stmt2->bindValue(1, new JustString(), PDO::PARAM_STR);
echo 'plain=', $stmt2->execute() ? $stmt2->fetchColumn() : 'fail', "\n";
?>
--EXPECTF--
%r(completed|threw)%r
alive
plain=ok
