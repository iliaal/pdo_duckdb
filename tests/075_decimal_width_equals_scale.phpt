--TEST--
pdo_duckdb: DECIMAL with width == scale renders without a leading integer zero
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// DuckDB writes the integer-part '0' only when the type has room for an integer
// digit: DECIMAL(4,2) renders 0.05, DECIMAL(2,2) renders .05. Both the scalar
// fast renderer and the nested direct renderer must match the engine exactly.
$cases = [
    "CAST('0.5' AS DECIMAL(1,1))",
    "CAST('-0.5' AS DECIMAL(1,1))",
    "CAST('0.0' AS DECIMAL(1,1))",
    "CAST('0.05' AS DECIMAL(2,2))",
    "CAST('-0.00001' AS DECIMAL(5,5))",
    "CAST('0.123456789' AS DECIMAL(9,9))",
    "CAST('0.00000000000000000000000000000000000001' AS DECIMAL(38,38))",
    // width > scale keeps the leading zero
    "CAST('0.00' AS DECIMAL(4,2))",
    "CAST('-0.05' AS DECIMAL(4,2))",
    "CAST('9.9' AS DECIMAL(2,1))",
    "CAST('0' AS DECIMAL(4,0))",
];

foreach ($cases as $expr) {
    $row = $db->query(
        "SELECT $expr AS s, CAST($expr AS VARCHAR) AS ws,"
        . " [$expr] AS l, CAST([$expr] AS VARCHAR) AS wl,"
        . " {'d': $expr} AS t, CAST({'d': $expr} AS VARCHAR) AS wt"
    )->fetch(PDO::FETCH_ASSOC);

    $ok = $row['s'] === $row['ws'] && $row['l'] === $row['wl'] && $row['t'] === $row['wt'];
    echo $ok ? "ok   {$row['ws']}\n"
        : "BAD  scalar={$row['s']}/{$row['ws']} list={$row['l']}/{$row['wl']} struct={$row['t']}/{$row['wt']}\n";
}
?>
--EXPECT--
ok   .5
ok   -.5
ok   .0
ok   .05
ok   -.00001
ok   .123456789
ok   .00000000000000000000000000000000000001
ok   0.00
ok   -0.05
ok   9.9
ok   0
