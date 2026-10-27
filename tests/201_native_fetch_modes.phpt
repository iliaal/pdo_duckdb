--TEST--
pdo_duckdb: typed native wrappers preserve PDO fetch modes, duplicate names, and ownership
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function checkValue($value, string $type, $payload): void {
    if (!$value instanceof stdClass || array_keys(get_object_vars($value)) !== ['type', 'value']
        || $value->type !== $type || $value->value !== $payload) {
        throw new Exception(var_export($value, true));
    }
}
class NativeRow { public $v; }
class NativeStatement extends PDOStatement { protected function __construct() {} }
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$row = $db->query("SELECT [1] AS v, [2] AS v, 'raw' AS v")->fetch(PDO::FETCH_NAMED);
if (array_keys($row) !== ['v'] || count($row['v']) !== 3) throw new Exception('duplicate accumulator');
checkValue($row['v'][0], 'INTEGER[]', [1]);
checkValue($row['v'][1], 'INTEGER[]', [2]);
if ($row['v'][2] !== 'raw') throw new Exception('duplicate scalar');
echo "duplicate names OK\n";
$sql = 'SELECT [1,2] AS v';
checkValue($db->query($sql)->fetch(PDO::FETCH_ASSOC)['v'], 'INTEGER[]', [1,2]);
checkValue($db->query($sql)->fetch(PDO::FETCH_OBJ)->v, 'INTEGER[]', [1,2]);
checkValue($db->query($sql)->fetchObject(NativeRow::class)->v, 'INTEGER[]', [1,2]);
checkValue($db->query($sql)->fetchAll(PDO::FETCH_CLASS, NativeRow::class)[0]->v, 'INTEGER[]', [1,2]);
checkValue($db->query($sql)->fetchAll(PDO::FETCH_COLUMN)[0], 'INTEGER[]', [1,2]);
$target = new NativeRow;
$st = $db->query($sql);
$st->setFetchMode(PDO::FETCH_INTO, $target);
if ($st->fetch() !== $target) throw new Exception('FETCH_INTO identity');
checkValue($target->v, 'INTEGER[]', [1,2]);
if ($db->query($sql)->fetchAll(PDO::FETCH_FUNC, fn(stdClass $v) => $v->value) !== [[1,2]]) throw new Exception('callback');
checkValue($db->query('SELECT 7 AS k, [1,2] AS v')->fetchAll(PDO::FETCH_KEY_PAIR)[7], 'INTEGER[]', [1,2]);
checkValue($db->query('SELECT 7 AS k, [1,2] AS v')->fetchAll(PDO::FETCH_GROUP | PDO::FETCH_ASSOC)[7][0]['v'], 'INTEGER[]', [1,2]);
checkValue($db->query('SELECT 7 AS k, [1,2] AS v')->fetchAll(PDO::FETCH_UNIQUE | PDO::FETCH_ASSOC)[7]['v'], 'INTEGER[]', [1,2]);
echo "fetch modes OK\n";
foreach ([PDO::FETCH_KEY_PAIR, PDO::FETCH_GROUP | PDO::FETCH_NUM, PDO::FETCH_UNIQUE | PDO::FETCH_NUM] as $mode) {
    $st = $db->query('SELECT [1] AS k, 42 AS v');
    try {
        $st->fetchAll($mode);
        throw new Exception('structured key silently coerced');
    } catch (Error $e) {
        if (!str_contains($e->getMessage(), 'stdClass')) throw $e;
    }
    $st = $db->query('SELECT [1] AS k, 42 AS v');
    $st->bindColumn(1, $key, PDO::PARAM_STR);
    $grouped = $st->fetchAll($mode);
    $expected = match ($mode) {
        PDO::FETCH_KEY_PAIR => ['[1]'=>42],
        PDO::FETCH_GROUP | PDO::FETCH_NUM => ['[1]'=>[[42]]],
        default => ['[1]'=>[42]],
    };
    if ($key !== '[1]' || $grouped !== $expected) throw new Exception('scalar-bound key');
}
echo "scalar group keys OK\n";
$row = $db->query($sql)->fetch(PDO::FETCH_BOTH);
if ($row[0] !== $row['v'] || ReflectionReference::fromArrayElement($row, 'v') !== null) throw new Exception('wrapper identity');
$copy = $row;
if ($copy['v'] !== $row['v']) throw new Exception('ordinary object copy');
$clone = clone $row['v'];
$clone->value[0] = 99;
checkValue($row['v'], 'INTEGER[]', [1,2]);
checkValue($clone, 'INTEGER[]', [99,2]);
$st = $db->query('SELECT [i::INTEGER] AS v FROM range(2) t(i)');
$lazy = $st->fetch(PDO::FETCH_LAZY);
$saved = $lazy->v;
checkValue($saved, 'INTEGER[]', [0]);
$st->fetch(PDO::FETCH_LAZY);
checkValue($lazy->v, 'INTEGER[]', [1]);
$st->closeCursor();
checkValue($saved, 'INTEGER[]', [0]);
$cycleStmt = $db->query('SELECT [1]');
$cycleValue = $cycleStmt->fetchColumn();
$cycleValue->owner = $cycleStmt;
$weakStmt = WeakReference::create($cycleStmt);
$weakValue = WeakReference::create($cycleValue);
unset($cycleStmt, $cycleValue);
gc_collect_cycles();
if ($weakStmt->get() !== null || $weakValue->get() !== null) throw new Exception('driver cache cycle');
echo "ownership OK\n";
$db->setAttribute(PDO::ATTR_STATEMENT_CLASS, [NativeStatement::class]);
$st = $db->query('SELECT [1] AS v, [2] AS v');
if (!$st instanceof NativeStatement) throw new Exception('custom statement replaced');
$row = $st->fetch(PDO::FETCH_NAMED);
checkValue($row['v'][0], 'INTEGER[]', [1]);
checkValue($row['v'][1], 'INTEGER[]', [2]);
if ($db->query('SELECT NULL::INTEGER[], 7')->fetch(PDO::FETCH_NUM) !== [null,7]) throw new Exception('scalar/null mapping');
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, true);
if ($db->query($sql)->fetchColumn() !== '[1, 2]') throw new Exception('stringify');
echo "custom statements and scalar mappings OK\n";
?>
--EXPECT--
duplicate names OK
fetch modes OK
scalar group keys OK
ownership OK
custom statements and scalar mappings OK
