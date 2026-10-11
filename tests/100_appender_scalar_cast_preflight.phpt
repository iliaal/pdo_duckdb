--TEST--
pdo_duckdb: Appender rejects uncastable non-string scalars before appending, keeping buffered rows
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// A failed native append invalidates the DuckDB appender and discards every
// buffered row, so each bad value must be rejected before any append.
$cases = [
    ['FLOAT', 1.0, 1e100],
    ['FLOAT', 1.0, -1e100],
    ['FLOAT', 1.0, 3.4028235677973366e38],
    ['DOUBLE', 1.0, null],
    ['INTEGER', 1.0, 3e9],
    ['INTEGER', 1.0, NAN],
    ['INTEGER', 1.0, 2147483648.0],
    ['SMALLINT', 1.0, 40000.0],
    ['UTINYINT', 1.0, -1.0],
    ['BIGINT', 1.0, 1e30],
    ['DECIMAL(4,1)', 1.5, 1e5],
    ['DECIMAL(4,1)', 1, 100000],
    ['DATE', '2026-01-01', 5],
    ['DATE', '2026-01-01', 1.5],
    ['DATE', '2026-01-01', true],
    ['TIMESTAMP', '2026-01-01 00:00:00', 5],
    ['UUID', '00000000-0000-0000-0000-000000000000', 5],
    ['BLOB', 'x', 5],
];
foreach ($cases as [$type, $good, $bad]) {
    $db = new PDO('duckdb::memory:');
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $db->exec("CREATE TABLE t (id INTEGER, v $type)");
    $app = $db->duckdbAppender('t');
    for ($i = 1; $i <= 3; $i++) {
        $app->appendRow($i, $good);
    }
    $err = 'accepted';
    if ($bad !== null) {
        try {
            $app->appendRow(4, $bad);
        } catch (Throwable $e) {
            $err = get_class($e);
        }
    }
    $app->appendRow(5, $good);
    $app->close();
    $ids = implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN));
    printf("%s %s: %s rows=%s\n", $type, var_export($bad, true), $err, $ids);
}

// Values at the edge of the fast-accept bounds still go through DuckDB's cast.
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE e (f FLOAT, i INTEGER, d DECIMAL(4,1), u UTINYINT)');
$app = $db->duckdbAppender('e');
$app->appendRow(3.4028235677973362e38, 2147483647.4, 999.94, 255.4);
$app->appendRow(INF, -2147483648.0, -999.9, 255.0);
$app->appendRow(NAN, 0.0, 0, 0);
$app->close();
foreach ($db->query('SELECT f::VARCHAR, i, d::VARCHAR, u FROM e')->fetchAll(PDO::FETCH_NUM) as $row) {
    echo implode('|', $row), "\n";
}
?>
--EXPECT--
FLOAT 1.0E+100: PDOException rows=1,2,3,5
FLOAT -1.0E+100: PDOException rows=1,2,3,5
FLOAT 3.4028235677973366E+38: PDOException rows=1,2,3,5
DOUBLE NULL: accepted rows=1,2,3,5
INTEGER 3000000000.0: PDOException rows=1,2,3,5
INTEGER NAN: PDOException rows=1,2,3,5
INTEGER 2147483648.0: PDOException rows=1,2,3,5
SMALLINT 40000.0: PDOException rows=1,2,3,5
UTINYINT -1.0: PDOException rows=1,2,3,5
BIGINT 1.0E+30: PDOException rows=1,2,3,5
DECIMAL(4,1) 100000.0: PDOException rows=1,2,3,5
DECIMAL(4,1) 100000: PDOException rows=1,2,3,5
DATE 5: PDOException rows=1,2,3,5
DATE 1.5: PDOException rows=1,2,3,5
DATE true: PDOException rows=1,2,3,5
TIMESTAMP 5: PDOException rows=1,2,3,5
UUID 5: PDOException rows=1,2,3,5
BLOB 5: TypeError rows=1,2,3,5
3.4028235e+38|2147483647|999.9|255
inf|-2147483648|-999.9|255
nan|0|0.0|0
