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
// Seeded sweep: every internal-width bucket (int16/int32/int64/hugeint) x
// every scale class (0, width == scale, width > scale), signs alternating
// (so every bucket/scale pair covers negatives, incl. DECIMAL(38,38)).
// Each value is checked driver-render vs engine CAST AS VARCHAR on the
// scalar, list and struct paths.
$specs = [
    [4, 0], [4, 4], [4, 2],
    [9, 0], [9, 9], [9, 2],
    [18, 0], [18, 18], [18, 5],
    [38, 0], [38, 38], [38, 10],
];
mt_srand(20260903);
$cases = 0; $bad = 0;
foreach ($specs as [$w, $s]) {
    for ($i = 0; $i < 20; $i++) {
        $int_len = $w - $s;
        // DECIMAL(18,0) rides int64: keep the lead digit <= 8 so the
        // literal can never exceed 9223372036854775807.
        $first_max = ($w === 18 && $s === 0) ? 8 : 9;
        $int = '';
        for ($d = 0; $d < $int_len; $d++) {
            $int .= (string) mt_rand($d === 0 && $int_len > 1 ? 1 : 0, $first_max);
        }
        $frac = '';
        for ($d = 0; $d < $s; $d++) {
            $frac .= (string) mt_rand(0, 9);
        }
        $lit = ($i % 2 ? '-' : '') . ($int !== '' ? $int : '0') . ($s > 0 ? ".$frac" : '');
        $expr = "CAST('$lit' AS DECIMAL($w,$s))";
        $row = $db->query(
            "SELECT $expr AS s, CAST($expr AS VARCHAR) AS ws,"
            . " [$expr] AS l, CAST([$expr] AS VARCHAR) AS wl,"
            . " {'d': $expr} AS t, CAST({'d': $expr} AS VARCHAR) AS wt"
        )->fetch(PDO::FETCH_ASSOC);
        $cases++;
        if (!($row['s'] === $row['ws'] && $row['l'] === $row['wl'] && $row['t'] === $row['wt'])) {
            $bad++;
            echo "BAD  $expr scalar={$row['s']}/{$row['ws']} list={$row['l']}/{$row['wl']} struct={$row['t']}/{$row['wt']}\n";
        }
    }
}
echo "sweep_cases=$cases sweep_bad=$bad\n";
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
sweep_cases=240 sweep_bad=0
