--TEST--
pdo_duckdb: integer division and modulo zero honor defaults, TRY, and compatibility options
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();

function expectError(callable $operation, string $message): void
{
    try {
        $operation();
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), $message)) {
            throw $e;
        }
        return;
    }
    throw new RuntimeException("Expected $message");
}

foreach (['default', 'dsn', 'array'] as $source) {
    $connect = fn() => new PDO(
        'duckdb::memory:' . ($source === 'dsn' ? ';error_on_division_by_zero=false' : ''),
        null,
        null,
        [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]
            + ($source === 'array' ? [PDO::DUCKDB_ATTR_CONFIG => ['error_on_division_by_zero' => 'false']] : [])
    );
    if (!$v2 && $source !== 'default') {
        expectError($connect, 'The following options were not recognized: error_on_division_by_zero');
    } else {
        $db = $connect();
        foreach (['//' => 3, '%' => 1] as $operator => $nonzero) {
            $literal = fn() => $db->query("SELECT 1 $operator 0")->fetchColumn();
            $stmt = $db->prepare("SELECT CAST(? AS INTEGER) $operator CAST(? AS INTEGER)");
            $prepared = function () use ($stmt) {
                $stmt->execute([1, 0]);
                return $stmt->fetchColumn();
            };
            foreach ([$literal, $prepared] as $operation) {
                if ($v2 && $source === 'default') {
                    expectError($operation, 'Invalid Input Error: Division by zero');
                } elseif ($operation() !== null) {
                    throw new RuntimeException("$source: $operator zero did not return NULL");
                }
            }
            $stmt->execute([7, 2]);
            if ($stmt->fetchColumn() !== $nonzero) {
                throw new RuntimeException("$operator with a nonzero denominator failed after reuse");
            }
        }
        if ($db->query('SELECT TRY(1 // 0), TRY(1 % 0)')->fetch(PDO::FETCH_NUM) !== [null, null]) {
            throw new RuntimeException('TRY did not return NULL for both zero expressions');
        }
        if ($db->query('SELECT 1 / 0')->fetchColumn() !== INF) {
            throw new RuntimeException('Floating division by zero did not return positive infinity');
        }
    }
    echo "$source: division-by-zero contract verified\n";
}
?>
--EXPECT--
default: division-by-zero contract verified
dsn: division-by-zero contract verified
array: division-by-zero contract verified
