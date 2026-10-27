--TEST--
pdo_duckdb: parameter scanning preserves DuckDB strings, identifiers, and nested comments
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$escaped = <<<'SQL'
SELECT E'a\'? :ignored', CAST(? AS INTEGER)
SQL;
$cases = [
    ['SELECT $$? :ignored$$, CAST(? AS INTEGER)', [17], ['? :ignored', 17]],
    ['SELECT $tag$? :ignored$wrong$ text$tag$, CAST(:x AS INTEGER)', ['x' => 17], ['? :ignored$wrong$ text', 17]],
    ['SELECT $é2$? :ignored$é2$, CAST(? AS INTEGER)', [17], ['? :ignored', 17]],
    ['SELECT 1 AS "quoted""? :ignored", CAST(? AS INTEGER)', [17], [1, 17]],
    ['SELECT 1 /* outer /* :ignored */ ? */ + CAST(:x AS INTEGER)', ['x' => 17], [18]],
    ["SELECT 1 -- :ignored ?\r + CAST(? AS INTEGER)", [17], [18]],
    ["SELECT 'a\\' || CAST(? AS VARCHAR)", ['x'], ["a\\x"]],
    ["SELECT 'a''? :ignored', ?::INTEGER", [17], ["a'? :ignored", 17]],
    [$escaped, [17], ["a'? :ignored", 17]],
    ['SELECT CAST(:x AS INTEGER) + CAST(:x AS INTEGER)', ['x' => 17], [34]],
    ['SELECT list_transform([1,2],lambda x:x+1), CAST(:x AS INTEGER)', ['x' => 17], ['[2, 3]', 17]],
];
foreach ($cases as $index => [$sql, $params, $expected]) {
    $stmt = $db->prepare($sql);
    $stmt->execute($params);
    $rows = $stmt->fetchAll(PDO::FETCH_NUM);
    if ($rows !== [$expected]) {
        throw new RuntimeException("Case $index: " . json_encode($rows));
    }
}
try {
    $db->prepare('SELECT CAST(:x AS INTEGER) + CAST(? AS INTEGER)');
    throw new RuntimeException('Mixed PDO placeholder styles were accepted');
} catch (PDOException $e) {
    if ($e->getCode() !== 'HY093' || !str_contains($e->getMessage(), 'mixed named and positional')) {
        throw $e;
    }
}
echo count($cases), " quoting cases and mixed-style rejection verified\n";
?>
--EXPECT--
11 quoting cases and mixed-style rejection verified
