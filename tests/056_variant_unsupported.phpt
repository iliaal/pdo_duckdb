--TEST--
pdo_duckdb: VARIANT fetch APIs preserve top-level and nested values in canonical and native modes
--EXTENSIONS--
pdo
pdo_duckdb
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
foreach ([false, true] as $native) {
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, $native);
    $label = $native ? 'native' : 'canonical';
    $value = $native ? (object)['type' => 'INTEGER', 'value' => 42] : '42';
    foreach ([
        [static fn(PDOStatement $st) => $st->fetch(PDO::FETCH_ASSOC), ['value' => $value]],
        [static fn(PDOStatement $st) => $st->fetchColumn(), $value],
        [static fn(PDOStatement $st) => $st->fetchAll(PDO::FETCH_ASSOC), [['value' => $value]]],
    ] as [$fetch, $expected]) {
        check($fetch($db->query('SELECT CAST(42 AS VARIANT) AS value')), $expected);
    }
    check($db->query('SELECT CAST(NULL AS VARIANT)')->fetchColumn(), null);
    check($db->query('SELECT CAST(CAST(42 AS VARIANT) AS VARCHAR)')->fetchColumn(), '42');
    check($db->query('SELECT 7')->fetchColumn(), 7);
    check($db->query("SELECT [NULL::VARIANT], struct_pack(v := NULL::VARIANT)")->fetch(PDO::FETCH_NUM),
        $native ? [(object)['type' => 'VARIANT[]', 'value' => [null]],
            (object)['type' => 'STRUCT(v VARIANT)', 'value' => ['v' => null]]] : ['[NULL]', "{'v': NULL}"]);
    echo "$label fetch APIs OK\n";

    $oneTag = ['type' => 'INTEGER', 'value' => 1];
    $one = $native ? (object)$oneTag : '1';
    $rows = 'SELECT i, CASE WHEN i = 1 THEN CAST(CAST(i AS INTEGER) AS VARIANT) END AS v FROM range(3) t(i)';
    check($db->query($rows)->fetchAll(PDO::FETCH_NUM), [[0, null], [1, $one], [2, null]]);
    $st = $db->query($rows);
    check([$st->fetchColumn(0), $st->fetchColumn(0), $st->fetchColumn(0)], [0, 1, 2]);
    check($db->query('SELECT [CAST(1 AS VARIANT)]')->fetchColumn(),
        $native ? (object)['type' => 'VARIANT[]', 'value' => [$oneTag]] : '[1]');
    echo "$label mixed rows OK\n";

    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
    $st = $db->query($rows);
    check([$st->fetch(PDO::FETCH_NUM), $st->fetch(PDO::FETCH_NUM), $st->fetch(PDO::FETCH_NUM)],
        [[0, null], [1, $one], [2, null]]);
    check($st->errorCode(), '00000');
    $st = $db->query('SELECT CAST(NULL AS VARIANT)');
    check($st->fetchColumn(), null);
    check($st->errorCode(), '00000');
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    echo "$label silent mode OK\n";
}
?>
--EXPECT--
canonical fetch APIs OK
canonical mixed rows OK
canonical silent mode OK
native fetch APIs OK
native mixed rows OK
native silent mode OK
