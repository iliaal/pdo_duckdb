--TEST--
pdo_duckdb: modern lambdas work on every engine and legacy arrows honor the compatibility option
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();

foreach (['default', 'dsn', 'array'] as $source) {
    $db = new PDO(
        'duckdb::memory:' . ($source === 'dsn' ? ';lambda_syntax=ENABLE_SINGLE_ARROW' : ''),
        null,
        null,
        [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]
            + ($source === 'array' ? [PDO::DUCKDB_ATTR_CONFIG => ['lambda_syntax' => 'ENABLE_SINGLE_ARROW']] : [])
    );
    foreach (['modern' => 'lambda x: x + 1', 'arrow' => 'x -> x + 1'] as $kind => $lambda) {
        $sql = "SELECT list_transform([1, 2, NULL], $lambda)";
        if ($v2 && $source === 'default' && $kind === 'arrow') {
            try {
                $db->query($sql);
                throw new RuntimeException('Default GA lambda arrow was accepted');
            } catch (PDOException $e) {
                if ($e->errorInfo[0] !== '42000' || $e->errorInfo[1] !== 3
                    || !str_contains($e->getMessage(), 'Binder Error: Deprecated lambda arrow (->) detected')) {
                    throw $e;
                }
            }
        } elseif ($db->query($sql)->fetchColumn() !== '[2, 3, NULL]') {
            throw new RuntimeException("$source/$kind lambda returned incorrect values");
        }
    }
    $stmt = $db->prepare('SELECT list_transform([CAST(? AS INTEGER), CAST(? AS INTEGER), NULL], lambda x: x + CAST(? AS INTEGER))');
    $stmt->execute([1, 5, 2]);
    if ($stmt->fetchColumn() !== '[3, 7, NULL]') {
        throw new RuntimeException('Modern lambda failed with PDO parameters');
    }
    if (!$v2 || $source !== 'default') {
        $stmt = $db->prepare('SELECT list_transform([CAST(? AS INTEGER), CAST(? AS INTEGER), NULL], x -> x + CAST(? AS INTEGER))');
        $stmt->execute([1, 5, 2]);
        if ($stmt->fetchColumn() !== '[3, 7, NULL]') {
            throw new RuntimeException('Legacy lambda failed with PDO parameters');
        }
    }
    echo "$source: lambda syntax and parameter results verified\n";
}
?>
--EXPECT--
default: lambda syntax and parameter results verified
dsn: lambda syntax and parameter results verified
array: lambda syntax and parameter results verified
