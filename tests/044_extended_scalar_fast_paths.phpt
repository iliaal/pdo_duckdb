--TEST--
pdo_duckdb: extended scalar canonical strings match DuckDB casts
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("SET TimeZone='UTC'");

$cases = [
    'timetz fractional offset seconds' => "TIMETZ '12:34:56.789123+02:30:15'",
    'timetz negative offset minutes' => "TIMETZ '12:34:56.789123-00:45'",
    'timestamptz normalizes utc' => "TIMESTAMPTZ '2024-02-29 12:34:56.789123+02:30'",
    'interval days time' => "INTERVAL '3 days 04:05:06'",
    'interval negative days time' => "INTERVAL '-3 days -04:05:06'",
    'interval fractional' => "INTERVAL '1 microsecond'",
    'interval hours overflow day boundary' => "INTERVAL '-25 hours'",
    'interval months fallback' => "INTERVAL '1 year 2 months 3 days 04:05:06.789123'",
    'bit partial byte' => "'101010'::BIT",
    'bit full byte' => "'10101010'::BIT",
];

foreach ($cases as $label => $expr) {
    $row = $db->query("SELECT $expr AS got, CAST($expr AS VARCHAR) AS want")->fetch(PDO::FETCH_ASSOC);
    echo $label, ': ', ($row['got'] === $row['want'] ? 'ok' : "MISMATCH {$row['got']} != {$row['want']}"), "\n";
}
?>
--EXPECT--
timetz fractional offset seconds: ok
timetz negative offset minutes: ok
timestamptz normalizes utc: ok
interval days time: ok
interval negative days time: ok
interval fractional: ok
interval hours overflow day boundary: ok
interval months fallback: ok
bit partial byte: ok
bit full byte: ok
