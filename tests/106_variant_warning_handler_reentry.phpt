--TEST--
pdo_duckdb: structured conversion errors reach reentrant handlers only after PDO core finishes the fetch
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
class Row {
    public $w;
    public $v;
    public $x;
    public $unbound;
    public function __construct(public $tag = null) {}
}

$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_WARNING,
    PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true,
]);

$actions = [
    'closeCursor' => static fn(PDOStatement $st) => $st->closeCursor(),
    'execute' => static fn(PDOStatement $st) => $st->execute(),
    'fetch' => static fn(PDOStatement $st) => $st->fetch(),
    'bindColumn' => static function (PDOStatement $st) { $x = null; return $st->bindColumn('x', $x); },
    'setFetchMode' => static fn(PDOStatement $st) => $st->setFetchMode(PDO::FETCH_ASSOC),
    'fetchObject' => static fn(PDOStatement $st) => $st->fetchObject('Row', ['inner']),
    'fetchAll' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_FUNC, static fn(...$c) => $c),
];
$fetches = [
    'assoc' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_ASSOC),
    'both' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_BOTH),
    'named' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_NAMED),
    'obj' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_OBJ),
    'all' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_ASSOC),
    'iter' => static function (PDOStatement $st) {
        $st->setFetchMode(PDO::FETCH_ASSOC);
        $rows = [];
        foreach ($st as $row) {
            $rows[] = $row;
        }
        return $rows;
    },
    'bound' => static function (PDOStatement $st) {
        $w = 'before-w';
        $v = 'before-v';
        $x = 'before-x';
        $st->bindColumn('w', $w);
        $st->bindColumn('v', $v);
        $st->bindColumn('x', $x);
        $result = $st->fetch(PDO::FETCH_BOUND);
        if ([$w, $v, $x] !== ['before-w', 'before-v', 'before-x']) {
            throw new Exception('failed row partially updated bound columns');
        }
        return $result;
    },
    'func' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_FUNC, static fn(...$c) => $c),
    'class' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_CLASS, 'Row', ['outer']),
    'fetchObject' => static fn(PDOStatement $st) => $st->fetchObject('Row', ['outer']),
];
$deep = "CAST('" . str_repeat('[', 130) . '0' . str_repeat(']', 130) . "' AS JSON)";
$queries = [
    'scalar' => "SELECT 2 AS w, $deep AS v, 3 AS x, $deep AS unbound",
    'nested' => "SELECT 2 AS w, struct_pack(payload := $deep) AS v, 3 AS x, struct_pack(payload := $deep) AS unbound",
];

foreach ($queries as $qlabel => $sql) {
    foreach ($actions as $alabel => $action) {
        foreach ($fetches as $flabel => $fetch) {
            /* PDO core's fetchObject() reports a fetch error before it restores
             * its class-fetch state, so fetchAll(PDO::FETCH_FUNC) from that
             * report crashes in core with any driver (pdo_sqlite included). */
            if ($alabel === 'fetchAll' && $flabel === 'fetchObject') {
                continue;
            }
            $st = $db->query($sql);
            $calls = 0;
            set_error_handler(static function (int $no, string $msg) use ($st, $action, &$calls) {
                if ($no !== E_WARNING || !str_contains($msg, 'maximum nesting depth')) {
                    throw new Exception('unexpected warning: ' . $msg);
                }
                $calls++;
                try {
                    @$action($st);
                } catch (Throwable $e) {
                }
                return true;
            });
            $result = $fetch($st);
            restore_error_handler();
            if ($result !== false && $result !== []) {
                echo "$qlabel/$alabel/$flabel: returned ", json_encode($result), "\n";
            }
            if ($calls !== 1) {
                echo "$qlabel/$alabel/$flabel: handler ran $calls times\n";
            }
        }
    }
}
echo "handler matrix done\n";

foreach (['func' => $fetches['func'], 'class' => $fetches['class']] as $flabel => $fetch) foreach ($actions as $alabel => $action) {
    $st = $db->query($queries['scalar']);
    $calls = 0;
    ob_start(static function (string $buf) use ($st, $action, &$calls) {
        if (str_contains($buf, 'maximum nesting depth')) {
            $calls++;
            try {
                @$action($st);
            } catch (Throwable $e) {
            }
        }
        return '';
    }, 1);
    $result = $fetch($st);
    ob_end_clean();
    echo "ob_start/$flabel/$alabel: ", json_encode($result), " callback hits ", $calls > 0 ? 'yes' : 'no', "\n";
}

$st = $db->query('SELECT 2 AS w, CAST(NULL AS VARIANT) AS v, [CAST(NULL AS VARIANT)] AS l');
$row = $st->fetch(PDO::FETCH_ASSOC);
if (array_keys($row) !== ['w', 'v', 'l'] || $row['w'] !== 2 || $row['v'] !== null
    || !is_object($row['l']) || get_class($row['l']) !== stdClass::class
    || get_object_vars($row['l']) !== ['type' => 'VARIANT[]', 'value' => [null]] || $st->errorCode() !== '00000') {
    throw new Exception('NULL VARIANT control failed');
}
$st = $db->query('SELECT CAST(1 AS VARIANT) AS v');
$row = $st->fetch(PDO::FETCH_ASSOC);
if (array_keys($row) !== ['v'] || !is_object($row['v']) || get_class($row['v']) !== stdClass::class
    || get_object_vars($row['v']) !== ['type' => 'INTEGER', 'value' => 1] || $st->errorCode() !== '00000') {
    throw new Exception('non-NULL VARIANT control failed');
}
echo "VARIANT controls OK\n";

$rows = "SELECT i, CASE WHEN i = 1 THEN $deep END AS v FROM range(3) t(i)";
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
foreach (['fetchAll', 'fetchColumn'] as $method) {
    $st = $db->query($rows);
    if ($method === 'fetchColumn' && $st->fetchColumn(0) !== 0) throw new Exception('first row changed');
    try {
        $method === 'fetchAll' ? $st->fetchAll(PDO::FETCH_NUM) : $st->fetchColumn(0);
        throw new Exception('later conversion failure was ignored');
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'maximum nesting depth')) throw $e;
        echo "$method rejects later row before get_col\n";
    }
}
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$st = $db->query($rows);
if ($st->fetch(PDO::FETCH_NUM) !== [0, null] || $st->fetch(PDO::FETCH_NUM) !== false
    || $st->errorCode() !== 'HY000' || !str_contains($st->errorInfo()[2], 'maximum nesting depth')
    || $st->fetch(PDO::FETCH_NUM) !== [2, null]) {
    throw new Exception('silent conversion failure changed row progress');
}
echo "silent conversion recovery OK\n";
?>
--EXPECTF--
handler matrix done
ob_start/func/closeCursor: [] callback hits yes
ob_start/func/execute: [] callback hits yes
ob_start/func/fetch: [] callback hits yes
ob_start/func/bindColumn: [] callback hits yes
ob_start/func/setFetchMode: [] callback hits yes
ob_start/func/fetchObject: [] callback hits yes
ob_start/func/fetchAll: [] callback hits yes
ob_start/class/closeCursor: [] callback hits yes
ob_start/class/execute: [] callback hits yes
ob_start/class/fetch: [] callback hits yes
ob_start/class/bindColumn: [] callback hits yes
ob_start/class/setFetchMode: [] callback hits yes
ob_start/class/fetchObject: [] callback hits yes
ob_start/class/fetchAll: [] callback hits yes
VARIANT controls OK
fetchAll rejects later row before get_col
fetchColumn rejects later row before get_col
silent conversion recovery OK
