--TEST--
pdo_duckdb: nested DATE/TIME/TIMESTAMP leaves quote exactly like CAST(... AS VARCHAR)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$cases = [
    "[TIMESTAMP '2020-01-01 10:00:00']",
    "{'t': TIME '12:00:00'}",
    "MAP {TIME '01:02:03': TIMESTAMP '2020-01-01 00:00:00.5'}",
    "[[TIME '24:00:00', NULL]]",
    "[DATE '2020-01-01', DATE '0044-03-15 (BC)', DATE 'infinity']",
    "[TIMESTAMP 'infinity', TIMESTAMP '-infinity']",
    "[TIMESTAMP '2020-01-01 10:00:00', TIMESTAMP '2020-01-01 11:00:00']::TIMESTAMP[2]",
    "[[TIME '12:00:00']::TIME[1]]",
    "[union_value(t := TIME '12:00:00')::UNION(n INTEGER, t TIME)]",
    "{'u': union_value(t := TIMESTAMP '2020-01-01 10:00:00')::UNION(n INTEGER, t TIMESTAMP)}",
];
foreach ($cases as $expr) {
    $row = $db->query("SELECT $expr AS got, CAST($expr AS VARCHAR) AS want")->fetch(PDO::FETCH_ASSOC);
    echo $row['got'], $row['got'] === $row['want'] ? '' : " != {$row['want']}", "\n";
}

$gen = [
    'DATE' => "DATE '1970-01-01' + ((i * 2654435761) % 4000000000 - 2000000000)::INTEGER",
    'TIMESTAMP' => "make_timestamp(((i * 2654435761 * 40503) % 18000000000000 - 9000000000000) * 1000000 + (i * 7919) % 1000000)",
    'TIME' => "TIME '00:00:00' + to_microseconds((i * 2654435761) % 86400000000)",
];
$shapes = [
    'list' => "[x, NULL]",
    'struct' => "{'a': x, 'b': [x]}",
    'map_key' => "MAP([x], [i])",
    'map_value' => "MAP([i], [x])",
    'array' => "[x]::%s[1]",
    'union' => "[union_value(t := x)::UNION(n INTEGER, t %s)]",
];
foreach ($gen as $type => $g) {
    foreach ($shapes as $name => $shape) {
        $expr = sprintf($shape, $type);
        $rows = $db->query(
            "SELECT $expr AS got, CAST($expr AS VARCHAR) AS want "
            . "FROM (SELECT ($g)::$type AS x, i FROM range(1, 501) r(i))"
        )->fetchAll(PDO::FETCH_NUM);
        $bad = 0;
        foreach ($rows as $r) {
            $bad += $r[0] !== $r[1];
        }
        printf("%s %s: %d rows, %d mismatches\n", $type, $name, count($rows), $bad);
    }
}
?>
--EXPECT--
['2020-01-01 10:00:00']
{'t': '12:00:00'}
{'01:02:03'='2020-01-01 00:00:00.5'}
[['24:00:00', NULL]]
[2020-01-01, '0044-03-15 (BC)', infinity]
[infinity, -infinity]
[2020-01-01 10:00:00, 2020-01-01 11:00:00]
[[12:00:00]]
[12:00:00]
{'u': 2020-01-01 10:00:00}
DATE list: 500 rows, 0 mismatches
DATE struct: 500 rows, 0 mismatches
DATE map_key: 500 rows, 0 mismatches
DATE map_value: 500 rows, 0 mismatches
DATE array: 500 rows, 0 mismatches
DATE union: 500 rows, 0 mismatches
TIMESTAMP list: 500 rows, 0 mismatches
TIMESTAMP struct: 500 rows, 0 mismatches
TIMESTAMP map_key: 500 rows, 0 mismatches
TIMESTAMP map_value: 500 rows, 0 mismatches
TIMESTAMP array: 500 rows, 0 mismatches
TIMESTAMP union: 500 rows, 0 mismatches
TIME list: 500 rows, 0 mismatches
TIME struct: 500 rows, 0 mismatches
TIME map_key: 500 rows, 0 mismatches
TIME map_value: 500 rows, 0 mismatches
TIME array: 500 rows, 0 mismatches
TIME union: 500 rows, 0 mismatches
