--TEST--
pdo_duckdb: unknown non-NULL types fail before get_col while supported VARIANT and NULL cells remain readable
--DESCRIPTION--
A type newer than the compile-time headers has an unknown vector layout. This
test runs only on a build configured with
CFLAGS=-DPDO_DUCKDB_TEST_UNDECODABLE_TYPE=13, which makes the driver treat
DATE (type id 13) as such a type.
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
ob_start();
(new ReflectionExtension('pdo_duckdb'))->info();
if (!str_contains(ob_get_clean(), 'Test-only undecodable type id => 13')) {
    die('skip needs a build with -DPDO_DUCKDB_TEST_UNDECODABLE_TYPE=13');
}
?>
--FILE--
<?php
function check($got, $want): void {
    if ($want instanceof stdClass) {
        if (!is_object($got) || get_class($got) !== stdClass::class) throw new Exception('expected stdClass wrapper');
        $got = get_object_vars($got);
        $want = get_object_vars($want);
    }
    if (is_array($want) && is_array($got) && array_keys($got) === array_keys($want)) {
        foreach ($want as $key => $value) check($got[$key], $value);
        return;
    }
    if ($got !== $want) throw new Exception(var_export([$got, $want], true));
}
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$error = 'DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL';
foreach ([false, true] as $native) {
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, $native);
    $label = $native ? 'native' : 'canonical';
    $variantTag = ['type' => 'INTEGER', 'value' => 42];
    $variant = $native ? (object)$variantTag : '42';
    foreach ([
        ['date', "SELECT DATE '2020-01-01' AS d", 'DATE', false],
        ['list', "SELECT 1 AS i, [DATE '2020-01-01'] AS l", 'LIST', false],
        ['struct', "SELECT struct_pack(d := DATE '2020-01-01') AS s", 'STRUCT', false],
        ['variant/null-date', 'SELECT 42::VARIANT AS v, NULL::DATE AS d', 'DATE', [$variant, null]],
        ['null-variant/date', "SELECT NULL::VARIANT AS v, DATE '2020-01-01' AS d", 'DATE', false],
        ['variant/date', "SELECT 42::VARIANT AS v, DATE '2020-01-01' AS d", 'DATE', false],
        ['struct-variant/null-date', 'SELECT struct_pack(v := 42::VARIANT, d := NULL::DATE) AS s', 'STRUCT',
            $native ? [(object)['type' => 'STRUCT(v VARIANT, d DATE)', 'value' => ['v' => $variantTag, 'd' => null]]]
                : ["{'v': 42, 'd': NULL}"]],
        ['struct-null-variant/date', "SELECT struct_pack(v := NULL::VARIANT, d := DATE '2020-01-01') AS s", 'STRUCT', false],
        ['struct-variant/date', "SELECT struct_pack(v := 42::VARIANT, d := DATE '2020-01-01') AS s", 'STRUCT', false],
    ] as [$name, $sql, $type, $expected]) {
        $st = $db->query($sql);
        if ($st->getColumnMeta($st->columnCount() - 1)['native_type'] !== $type) throw new Exception('metadata changed');
        try {
            $got = $st->fetch(PDO::FETCH_NUM);
            if ($expected === false) throw new Exception("$label/$name unexpected row: " . var_export($got, true));
            check($got, $expected);
            if ($st->errorCode() !== '00000') throw new Exception('successful row reported an error');
            echo "$label $name OK\n";
        } catch (PDOException $e) {
            if ($expected !== false || $st->errorInfo()[2] !== $error) throw $e;
            echo "$label $name rejected\n";
        }
    }
    check($db->query('SELECT NULL::DATE AS d, [NULL::DATE] AS l')->fetch(PDO::FETCH_NUM),
        $native ? [null, (object)['type' => 'DATE[]', 'value' => [null]]] : [null, '[NULL]']);
    if ($db->query("SELECT CAST(DATE '2020-01-01' AS VARCHAR)")->fetchColumn() !== '2020-01-01') {
        throw new Exception('explicit VARCHAR cast changed');
    }
    echo "$label NULL and cast controls OK\n";

    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
    $st = $db->query("SELECT i, CASE WHEN i = 1 THEN DATE '2020-01-01' END AS d FROM range(3) t(i)");
    if ($st->fetch(PDO::FETCH_NUM) !== [0, null] || $st->fetch(PDO::FETCH_NUM) !== false
        || $st->errorCode() !== 'HY000' || $st->errorInfo()[2] !== $error
        || $st->fetch(PDO::FETCH_NUM) !== [2, null]) {
        throw new Exception('silent unknown-type failure changed row progress');
    }
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    echo "$label silent failure recovery OK\n";
}
?>
--EXPECT--
canonical date rejected
canonical list rejected
canonical struct rejected
canonical variant/null-date OK
canonical null-variant/date rejected
canonical variant/date rejected
canonical struct-variant/null-date OK
canonical struct-null-variant/date rejected
canonical struct-variant/date rejected
canonical NULL and cast controls OK
canonical silent failure recovery OK
native date rejected
native list rejected
native struct rejected
native variant/null-date OK
native null-variant/date rejected
native variant/date rejected
native struct-variant/null-date OK
native struct-null-variant/date rejected
native struct-variant/date rejected
native NULL and cast controls OK
native silent failure recovery OK
