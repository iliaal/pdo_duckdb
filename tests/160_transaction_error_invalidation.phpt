--TEST--
pdo_duckdb: transaction errors retain or discard writes according to the engine policy
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

$cases = [
    'parser' => ['SELEC 1', 'Parser Error:'],
    'catalog' => ['SELECT * FROM missing_table', 'Catalog Error:'],
    'constraint' => ["INSERT INTO t VALUES (1, 'duplicate')", 'Constraint Error:'],
];
foreach (['exec', 'prepared'] as $entry) {
    foreach ($cases as $kind => [$sql, $message]) {
        foreach (['commit', 'rollBack'] as $finish) {
            $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
            $db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, payload VARCHAR)');
            $db->exec("INSERT INTO t VALUES (0, 'committed')");
            $token = bin2hex(random_bytes(8));
            $before = [[0, 'committed']];
            $pending = [...$before, [1, $token]];
            if (!$db->beginTransaction() || !$db->inTransaction()) {
                throw new RuntimeException('Transaction did not start');
            }
            $db->prepare('INSERT INTO t VALUES (1, ?)')->execute([$token]);
            if ($db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $pending) {
                throw new RuntimeException('Pending insert was not visible before the error');
            }
            expectError(function () use ($db, $entry, $sql): void {
                if ($entry === 'exec') {
                    $db->exec($sql);
                } else {
                    $db->prepare($sql)->execute();
                }
            }, $message);
            if (!$db->inTransaction()) {
                throw new RuntimeException('Failed statement unexpectedly ended the transaction');
            }

            $aborted = $v2 || $kind === 'constraint';
            if ($aborted) {
                $error = 'TransactionContext Error: Current transaction is aborted (please ROLLBACK)';
                expectError(fn() => $db->query('SELECT * FROM t'), $error);
                expectError(fn() => $db->exec("INSERT INTO t VALUES (2, 'after error')"), $error);
            } else {
                if ($db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $pending) {
                    throw new RuntimeException('Syntactic error discarded a legacy pending write');
                }
                $db->exec("INSERT INTO t VALUES (2, 'after error')");
                $pending[] = [2, 'after error'];
            }
            if ($db->$finish() !== true || $db->inTransaction()) {
                throw new RuntimeException("$finish did not end the transaction successfully");
            }
            $expected = $aborted || $finish === 'rollBack' ? $before : $pending;
            $actual = $db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM);
            if ($actual !== $expected) {
                throw new RuntimeException("$entry/$kind/$finish rows: " . json_encode($actual));
            }
            $db->beginTransaction();
            $db->prepare('INSERT INTO t VALUES (3, ?)')->execute([$token]);
            if (!$db->commit()) {
                throw new RuntimeException('Recovery commit failed');
            }
            $expected[] = [3, $token];
            if ($db->query('SELECT * FROM t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $expected) {
                throw new RuntimeException('Connection did not recover after ending the failed transaction');
            }
        }
        echo "$entry/$kind: errors, commit, rollback, and recovery verified\n";
    }
}
?>
--EXPECT--
exec/parser: errors, commit, rollback, and recovery verified
exec/catalog: errors, commit, rollback, and recovery verified
exec/constraint: errors, commit, rollback, and recovery verified
prepared/parser: errors, commit, rollback, and recovery verified
prepared/catalog: errors, commit, rollback, and recovery verified
prepared/constraint: errors, commit, rollback, and recovery verified
