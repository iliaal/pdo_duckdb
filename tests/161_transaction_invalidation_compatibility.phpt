--TEST--
pdo_duckdb: the GA transaction compatibility policy preserves syntactic-error writes only
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
$key = 'default_transaction_invalidation_policy';
$policy = 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE';

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

foreach (['dsn', 'array'] as $source) {
    $connect = fn() => new PDO(
        'duckdb::memory:' . ($source === 'dsn' ? ";$key=$policy" : ''),
        null,
        null,
        [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]
            + ($source === 'array' ? [PDO::DUCKDB_ATTR_CONFIG => [$key => $policy]] : [])
    );
    if (!$v2) {
        expectError($connect, "The following options were not recognized: $key");
    } else {
        foreach ([
            'parser' => ['SELEC 1', 'Parser Error:'],
            'catalog' => ['SELECT * FROM missing_table', 'Catalog Error:'],
            'constraint' => ["INSERT INTO t VALUES (1, 'duplicate')", 'Constraint Error:'],
        ] as $kind => [$sql, $message]) {
            $db = $connect();
            $db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, payload VARCHAR)');
            $db->exec("INSERT INTO t VALUES (0, 'committed')");
            $token = bin2hex(random_bytes(8));
            $before = [[0, 'committed']];
            $pending = [...$before, [1, $token]];
            $db->beginTransaction();
            $db->prepare('INSERT INTO t VALUES (1, ?)')->execute([$token]);
            if ($db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $pending) {
                throw new RuntimeException('Pending insert was not visible before the error');
            }
            expectError(fn() => $db->exec($sql), $message);
            if (!$db->inTransaction()) {
                throw new RuntimeException('Failed statement unexpectedly ended the transaction');
            }
            if ($kind === 'constraint') {
                expectError(
                    fn() => $db->query('SELECT * FROM t'),
                    'TransactionContext Error: Current transaction is aborted (please ROLLBACK)'
                );
                $expected = $before;
            } else {
                if ($db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $pending) {
                    throw new RuntimeException('Compatibility policy discarded a syntactic-error write');
                }
                $db->exec("INSERT INTO t VALUES (2, 'after error')");
                $expected = [...$pending, [2, 'after error']];
            }
            if ($db->commit() !== true || $db->inTransaction()) {
                throw new RuntimeException('Commit did not end the transaction successfully');
            }
            $actual = $db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM);
            if ($actual !== $expected) {
                throw new RuntimeException("$source/$kind committed rows: " . json_encode($actual));
            }
        }
    }
    echo "$source: version-specific compatibility contract verified\n";
}
?>
--EXPECT--
dsn: version-specific compatibility contract verified
array: version-specific compatibility contract verified
