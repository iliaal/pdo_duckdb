--TEST--
pdo_duckdb: 1 BC temporal values use DuckDB canonical strings
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$cases = [
    'date-ad' => "DATE '0001-01-01'",
    'date-1bc' => "DATE '0000-01-01'",
    'date-2bc' => "DATE '-0001-01-01'",
    'timestamp-1bc' => "TIMESTAMP '0000-01-01 12:34:56.123456'",
    'list-1bc' => "[DATE '0000-01-01', NULL]",
    'struct-1bc' => "{'d': DATE '0000-01-01'}",
];

foreach ($cases as $label => $expression) {
    $row = $db->query("SELECT $expression AS got, CAST($expression AS VARCHAR) AS want")
        ->fetch(PDO::FETCH_ASSOC);
    echo $label, ': ', $row['got'] === $row['want'] ? 'ok' : "MISMATCH {$row['got']} != {$row['want']}", "\n";
}
?>
--EXPECT--
date-ad: ok
date-1bc: ok
date-2bc: ok
timestamp-1bc: ok
list-1bc: ok
struct-1bc: ok
