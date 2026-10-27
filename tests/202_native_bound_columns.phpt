--TEST--
pdo_duckdb: scalar bound columns bypass native conversion in every row view
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$json = str_repeat('[', 130) . '0' . str_repeat(']', 130);
foreach ([PDO::PARAM_STR, PDO::PARAM_INT, PDO::PARAM_BOOL, PDO::PARAM_NULL, PDO::PARAM_LOB] as $type) {
    $st = $db->prepare('SELECT CAST(? AS JSON) AS deep, [1,2] AS items');
    $bound = null;
    $st->bindColumn('deep', $bound, $type);
    foreach ([PDO::FETCH_BOUND, PDO::FETCH_NUM] as $mode) {
        $st->execute([$json]);
        $row = $st->fetch($mode);
        $expected = match ($type) {
            PDO::PARAM_INT => 0, PDO::PARAM_BOOL => true, PDO::PARAM_NULL => null,
            default => $json,
        };
        $actual = $type === PDO::PARAM_LOB ? stream_get_contents($bound) : $bound;
        if ($actual !== $expected) throw new Exception("bound coercion $type");
        if ($mode === PDO::FETCH_BOUND) {
            if ($row !== true) throw new Exception('bound fetch');
        } else {
            if ($row[0] !== $json || !$row[1] instanceof stdClass
                || $row[1]->type !== 'INTEGER[]' || $row[1]->value !== [1,2]) {
                throw new Exception('per-column override');
            }
        }
    }
}
echo "bound types and reexecution OK\n";
$st = $db->prepare('SELECT CAST(? AS JSON), [1]');
$st->execute([$json]);
$st->bindColumn(1, $text, PDO::PARAM_STR);
$row = $st->fetch(PDO::FETCH_NUM);
if ($text !== $json || $row[0] !== $json || $row[1]->value !== [1]) throw new Exception('numeric binding');
echo "numeric binding OK\n";
$st = $db->prepare('SELECT [1] AS shallow, CAST(? AS JSON) AS deep');
$st->execute([$json]);
$untouched = 'sentinel';
$st->bindColumn('shallow', $untouched, PDO::PARAM_STR);
try {
    $st->fetch(PDO::FETCH_NUM);
    throw new Exception('unbound deep column accepted');
} catch (PDOException $e) {
    if (!str_contains($e->getMessage(), 'maximum nesting depth') || $untouched !== 'sentinel') throw $e;
}
echo "unbound failure remains atomic OK\n";
?>
--EXPECT--
bound types and reexecution OK
numeric binding OK
unbound failure remains atomic OK
