--TEST--
pdo_duckdb: column type mapping (int, double, bool, hugeint, ubigint, decimal, temporal)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$row = $db->query("SELECT
    42::BIGINT                                      AS big,
    7::INTEGER                                      AS i,
    TRUE                                            AS b,
    2.5::DOUBLE                                     AS dbl,
    170141183460469231731687303715884105727::HUGEINT AS h,
    18446744073709551615::UBIGINT                   AS ub,
    3.14::DECIMAL(10,2)                             AS dec,
    DATE '2026-06-18'                               AS d,
    TIME '12:34:56'                                 AS t,
    TIMESTAMP '2026-06-18 12:34:56'                 AS ts,
    NULL::INTEGER                                   AS n
")->fetch(PDO::FETCH_ASSOC);

foreach ($row as $k => $v) {
    printf("%-4s %s(%s)\n", $k, gettype($v), var_export($v, true));
}
?>
--EXPECT--
big  integer(42)
i    integer(7)
b    integer(1)
dbl  double(2.5)
h    string('170141183460469231731687303715884105727')
ub   string('18446744073709551615')
dec  string('3.14')
d    string('2026-06-18')
t    string('12:34:56')
ts   string('2026-06-18 12:34:56')
n    NULL(NULL)
