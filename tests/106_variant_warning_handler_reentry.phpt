--TEST--
pdo_duckdb: a VARIANT fetch error reaches userland only after PDO core finishes the fetch
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
class Row {
    public $w;
    public $v;
    public $x;
    public function __construct(public $tag = null) {}
}

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_WARNING]);

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
        $w = $v = $x = null;
        $st->bindColumn('w', $w);
        $st->bindColumn('v', $v);
        $st->bindColumn('x', $x);
        return $st->fetch(PDO::FETCH_BOUND);
    },
    'func' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_FUNC, static fn(...$c) => $c),
    'class' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_CLASS, 'Row', ['outer']),
    'fetchObject' => static fn(PDOStatement $st) => $st->fetchObject('Row', ['outer']),
];
$queries = [
    'scalar' => 'SELECT 2 AS w, CAST(1 AS VARIANT) AS v, 3 AS x',
    'nested' => 'SELECT 2 AS w, [CAST(1 AS VARIANT)] AS v, 3 AS x',
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
        if (str_contains($buf, 'VARIANT')) {
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
var_dump($st->fetch(PDO::FETCH_ASSOC), $st->errorCode());
$st = $db->query('SELECT CAST(1 AS VARIANT) AS v');
var_dump($st->fetch(PDO::FETCH_ASSOC), $st->errorInfo()[2]);
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
array(3) {
  ["w"]=>
  int(2)
  ["v"]=>
  NULL
  ["l"]=>
  string(6) "[NULL]"
}
string(5) "00000"

Warning: PDOStatement::fetch(): SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL in %s on line %d
bool(false)
string(93) "DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL"
