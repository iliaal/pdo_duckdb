--TEST--
pdo_duckdb: Appender VARIANT input preserves scalar kinds, arrays, objects, and length-aware keys
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->exec('CREATE TABLE variants (id INTEGER, v VARIANT, items VARIANT[], s STRUCT(v VARIANT))');
$keys = new stdClass;
$keys->{''} = 'empty';
$keys->{"a\0b"} = "x\0y";
$keys->{'0'} = 7;
$values = [
    null, true, false, 42, -9, 1.25, '{"x":1}', [], (object)[],
    [1, 'two', null, false, ['nested' => [3]]],
    ['answer' => 42, 'empty' => (object)[]],
    [0 => 'zero', 2 => 'two'],
    $keys,
    (object)['type' => 'DECIMAL(4,2)', 'value' => '1.20'],
    1.0,
];
$app = $db->duckdbAppender('variants');
foreach ($values as $id => $value) {
    $app->appendRow($id, $value, [$value], ['v' => $value]);
}
$app->close();
function canonical(mixed $value): mixed {
    if ($value instanceof stdClass) {
        $properties = get_object_vars($value);
        ksort($properties, SORT_STRING);
        $copy = new stdClass;
        foreach ($properties as $name => $property) $copy->{$name} = canonical($property);
        return $copy;
    }
    if (is_array($value)) return array_map('canonical', $value);
    return $value;
}
function normalizedJson(string $json): string {
    return json_encode(canonical(json_decode($json, false, 512, JSON_THROW_ON_ERROR)), JSON_THROW_ON_ERROR);
}
foreach ($db->query('SELECT id, v::JSON, items[1]::JSON, s.v::JSON,
        v IS NULL, items[1] IS NULL, s.v IS NULL, array_length(items),
        variant_typeof(v), variant_typeof(items[1]), variant_typeof(s.v)
        FROM variants ORDER BY id', PDO::FETCH_NUM) as $row) {
    [$id, $value, $item, $field, $isNull, $itemIsNull, $fieldIsNull, $length, $kind, $itemKind, $fieldKind] = $row;
    $expected = normalizedJson(json_encode($values[$id], JSON_THROW_ON_ERROR));
    if ($length !== 1) throw new Exception('VARIANT array length changed');
    $expectedKind = match (true) {
        $values[$id] === null => 'VARIANT_NULL',
        $values[$id] === true => 'BOOL_TRUE',
        $values[$id] === false => 'BOOL_FALSE',
        is_int($values[$id]) => 'INT64',
        is_float($values[$id]) => 'DOUBLE',
        is_string($values[$id]) => 'VARCHAR',
        default => null,
    };
    if ($expectedKind !== null && ($kind !== $expectedKind || $itemKind !== $expectedKind || $fieldKind !== $expectedKind)) {
        throw new Exception("VARIANT scalar kind changed at $id: $kind / $itemKind / $fieldKind");
    }
    if ($values[$id] === null) {
        if ($isNull !== 1 || $itemIsNull !== 1 || $fieldIsNull !== 1) throw new Exception('SQL NULL changed');
    } else {
        if ($isNull !== 0 || $itemIsNull !== 0 || $fieldIsNull !== 0) throw new Exception('value became SQL NULL');
        if (normalizedJson($value) !== $expected || normalizedJson($item) !== $expected || normalizedJson($field) !== $expected) {
            throw new Exception("VARIANT value mismatch at $id: $value / $item / $field / $expected");
        }
    }
    echo $id, ':', $isNull ? 'SQL NULL' : normalizedJson($value), "\n";
}
?>
--EXPECT--
0:SQL NULL
1:true
2:false
3:42
4:-9
5:1.25
6:"{\"x\":1}"
7:[]
8:{}
9:[1,"two",null,false,{"nested":[3]}]
10:{"answer":42,"empty":{}}
11:{"0":"zero","2":"two"}
12:{"":"empty","0":7,"a\u0000b":"x\u0000y"}
13:{"type":"DECIMAL(4,2)","value":"1.20"}
14:1
