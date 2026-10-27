--TEST--
pdo_duckdb: runtime legacy parser selection preserves deferred parameters and batches
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_remote.inc';
duckdb_remote_skipif();
try {
    duckdb_remote_connection()->exec('LOAD legacy_parser');
} catch (PDOException $e) {
    die('skip legacy_parser extension not available: ' . strtok($e->getMessage(), "\n"));
}
?>
--FILE--
<?php
require __DIR__ . '/duckdb_remote.inc';

function checkParameters(PDO $db, string $sql, array $values, array $expected): void
{
    $stmt = $db->prepare($sql);
    $stmt->execute($values);
    if ($stmt->fetchAll(PDO::FETCH_NUM) !== [$expected]) {
        throw new RuntimeException("Parser changed parameter interpretation: $sql");
    }
}

$remote = new DuckdbRemoteFixture();
$local = duckdb_remote_connection();
try {
    foreach ([$local, $remote->client] as $db) {
        $db->exec('LOAD legacy_parser');
    }
    foreach ([
        ['strict', true, 'strict', true],
        ['fallback', true, 'fallback', true],
        ['default', true, 'default', false],
        ['disabled', false, 'strict', false],
    ] as [$label, $enabled, $override, $legacy]) {
        foreach ([$remote->client, $local] as $db) {
            $db->exec('SET enable_legacy_parser=' . ($enabled ? 'true' : 'false'));
            $db->exec("SET allow_parser_override_extension='$override'");
            checkParameters($db, "SELECT :p || :p, ':ignored ?', /* :ignored ? */ :p",
                ['p' => "a'\\b"], ["a'\\ba'\\b", ':ignored ?', "a'\\b"]);
            if ($legacy) {
                foreach ([
                    ['SELECT $1_0, $1', [3], ['3', '3']],
                    ['SELECT $0, $0, $1', [3, 4], ['3', '4', '3']],
                    ['SELECT ?, $0', [3, 4], ['3', '4']],
                    ['SELECT $0, ?', [3], ['3', '3']],
                    ['SELECT ?, $0, ?', [3, 4], ['3', '4', '4']],
                    ['SELECT $1_0, ?', [3], ['3', '3']],
                ] as [$sql, $values, $expected]) {
                    checkParameters($db, $sql, $values, $expected);
                }
            } else {
                checkParameters($db, 'SELECT $1_0, $1, $2, $3, $4, $5, $6, $7, $8, $9',
                    range(1, 10), ['10', '1', '2', '3', '4', '5', '6', '7', '8', '9']);
            }
            if ($override === 'fallback' || !$legacy) {
                checkParameters($db, 'SELECT $1_0 AS `v?:ignored`, $1, $2, $3, $4, $5, $6, $7, $8, $9',
                    range(1, 10), ['10', '1', '2', '3', '4', '5', '6', '7', '8', '9']);
                checkParameters($db, 'SELECT :p AS `v?:ignored`, :p', ['p' => "a\0b"], ["a\0b", "a\0b"]);
            }
            $missing = $db->prepare('SELECT :p, :p');
            foreach ([[], ['p' => 3, 'extra' => 4]] as $values) {
                try {
                    $missing->execute($values);
                    throw new RuntimeException('Parser mode accepted a missing or excess binding');
                } catch (PDOException $e) {
                    if ($e->getCode() !== 'HY000' || !str_contains($e->getMessage(), 'param')) {
                        throw $e;
                    }
                }
            }
            $db->exec('CREATE OR REPLACE TABLE parser_batch(v INTEGER)');
            if ($db->exec("/* first */ INSERT INTO parser_batch VALUES (1);\n -- next\n INSERT INTO parser_batch VALUES (2)") !== 1
                || $db->exec('SELECT 42; /* next */ INSERT INTO parser_batch VALUES (3)') !== 0
                || $db->exec('BEGIN; /* write */ INSERT INTO parser_batch VALUES (4); COMMIT') !== 0
                || $db->inTransaction()
                || $db->query('SELECT v FROM parser_batch ORDER BY v')->fetchAll(PDO::FETCH_COLUMN) !== [1, 2, 3, 4]) {
                throw new RuntimeException('Parser mode lost or repeated a batch statement');
            }
        }
        $remote->server->exec('DELETE FROM remote_counts');
        $insert = $remote->client->prepare('INSERT INTO q.remote_counts VALUES (:id, :id)');
        if ($remote->server->query('SELECT count(*) FROM remote_counts')->fetchColumn() !== 0) {
            throw new RuntimeException('Legacy parser executed remote INSERT during prepare');
        }
        $insert->execute(['id' => 7]);
        if ($insert->rowCount() !== 1
            || $remote->server->query('SELECT id, v FROM remote_counts')->fetchAll(PDO::FETCH_NUM) !== [[7, 7]]) {
            throw new RuntimeException('Legacy parser changed a repeated remote binding');
        }
        echo "$label: native, deferred, fallback, and batch semantics verified\n";
    }
} finally {
    $remote->stop();
}
?>
--EXPECT--
strict: native, deferred, fallback, and batch semantics verified
fallback: native, deferred, fallback, and batch semantics verified
default: native, deferred, fallback, and batch semantics verified
disabled: native, deferred, fallback, and batch semantics verified
