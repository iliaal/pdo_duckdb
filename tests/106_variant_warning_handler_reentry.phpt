--TEST--
pdo_duckdb: an error handler cannot tear down a statement while get_col reports a VARIANT cell
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_WARNING]);

$actions = [
    'closeCursor' => static fn(PDOStatement $st) => $st->closeCursor(),
    'execute' => static fn(PDOStatement $st) => $st->execute(),
    'fetch' => static fn(PDOStatement $st) => $st->fetch(),
    'bindColumn' => static function (PDOStatement $st) { $x = null; return $st->bindColumn('x', $x, PDO::PARAM_INT); },
];
$fetches = [
    'assoc' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_ASSOC),
    'both' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_BOTH),
    'named' => static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_NAMED),
    'obj' => static fn(PDOStatement $st) => (array)$st->fetch(PDO::FETCH_OBJ),
    'all' => static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_ASSOC)[0],
    'iter' => static function (PDOStatement $st) {
        $st->setFetchMode(PDO::FETCH_ASSOC);
        foreach ($st as $row) {
            return $row;
        }
    },
    'bound' => static function (PDOStatement $st) {
        $w = $v = $x = 'unset';
        $st->bindColumn('w', $w, PDO::PARAM_INT);
        $st->bindColumn('v', $v);
        $st->bindColumn('x', $x, PDO::PARAM_INT);
        $st->fetch(PDO::FETCH_BOUND);
        return ['w' => $w, 'v' => $v, 'x' => $x];
    },
];
$queries = [
    'scalar' => 'SELECT 2 AS w, CAST(1 AS VARIANT) AS v, 3 AS x',
    'nested' => 'SELECT 2 AS w, [CAST(1 AS VARIANT)] AS v, 3 AS x',
];

foreach ($queries as $qlabel => $sql) {
    foreach ($actions as $alabel => $action) {
        foreach ($fetches as $flabel => $fetch) {
            /* PHP 8.5+ fetchAll() re-raises a still-set error code and discards
             * the rows; only bindColumn() leaves the VARIANT code in place. */
            if ($alabel === 'bindColumn' && $flabel === 'all') {
                continue;
            }
            $st = $db->query($sql);
            $seen = [];
            set_error_handler(static function (int $no, string $msg) use ($st, $action, &$seen) {
                try {
                    @$action($st);
                    $seen[] = 'allowed';
                } catch (Error $e) {
                    $seen[] = $e->getMessage();
                }
                return true;
            });
            $row = $fetch($st);
            restore_error_handler();
            $ok = $row['w'] === 2 && $row['v'] === null && $row['x'] === 3;
            echo "$qlabel/$alabel/$flabel: ", $ok ? 'row intact' : json_encode($row), '; ',
                implode(' | ', array_unique($seen)), "\n";
            var_dump($st->closeCursor());
        }
    }
}

$st = $db->query('SELECT CAST(1 AS VARIANT) AS v');
var_dump($st->fetch(PDO::FETCH_ASSOC), $st->errorInfo()[2]);
$st->closeCursor();
$st->execute();
var_dump($st->fetchColumn());
?>
--EXPECTF--
scalar/closeCursor/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/closeCursor/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/execute/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/fetch/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
scalar/bindColumn/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/closeCursor/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/execute/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/all: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/fetch/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/assoc: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/both: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/named: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/obj: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/iter: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)
nested/bindColumn/bound: row intact; Cannot use a PDOStatement while it is reporting a column fetch error
bool(true)

Warning: PDOStatement::fetch(): SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL in %s on line %d
array(1) {
  ["v"]=>
  NULL
}
string(93) "DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL"

Warning: PDOStatement::fetchColumn(): SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL in %s on line %d
NULL
