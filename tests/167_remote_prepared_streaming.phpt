--TEST--
pdo_duckdb: remote prepared SELECT streams lazily and preserves a rejected interleave
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_remote.inc';
duckdb_remote_skipif();
?>
--FILE--
<?php
require __DIR__ . '/duckdb_remote.inc';
$remote = new DuckdbRemoteFixture();
$client = $remote->client;
$remote->server->exec('SET threads=1');
try {
    $client->exec('CONNECT q');
    $client->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
    $lazy = $client->prepare("SELECT CAST(CASE WHEN i=100000000 THEN ? ELSE i::VARCHAR END AS BIGINT) FROM range(100000001) t(i)");
    $lazy->execute(['invalid integer']);
    if ($lazy->fetchColumn() !== 0 || !$lazy->closeCursor()) {
        throw new RuntimeException('Requested streaming query did not return before its late conversion error');
    }
    $stmt = $client->prepare('SELECT i FROM range(CAST(? AS INTEGER)) t(i)');
    $stmt->execute([5000]);
    if ($stmt->fetchColumn() !== 0) {
        throw new RuntimeException('First streamed row was lost');
    }
    foreach ([
        fn() => $client->query('SELECT 99'),
        fn() => $client->prepare('SELECT 99'),
        fn() => $client->prepare('SELECT 1 FROM'),
    ] as $interleaved) {
        try {
            $interleaved();
            throw new RuntimeException('Interleaved query or prepare was accepted');
        } catch (PDOException $e) {
            if (!str_contains($e->getMessage(), 'open result')) {
                throw $e;
            }
        }
    }
    $seen = 1;
    while (($value = $stmt->fetchColumn()) !== false) {
        if ($value !== $seen++) {
            throw new RuntimeException('Rejected interleave damaged the original stream');
        }
    }
    if ($seen !== 5000 || $stmt->rowCount() !== 0 || $stmt->errorCode() !== '00000') {
        throw new RuntimeException('Stream did not finish normally');
    }
    $stmt->execute([5000]);
    $stmt->fetchColumn();
    $stmt->closeCursor();
    if ($client->query('SELECT 42')->fetchAll(PDO::FETCH_COLUMN) !== [42]) {
        throw new RuntimeException('closeCursor did not release the remote stream');
    }
    $insert = $client->prepare('INSERT INTO remote_t VALUES (?, NULL, NULL, NULL)');
    $insert->execute([7]);
    if ($insert->rowCount() !== 1 || $remote->server->query('SELECT id FROM remote_t')->fetchColumn() !== 7) {
        throw new RuntimeException('Unbuffered DML did not commit at execute');
    }
    echo "lazy execution, preserved stream, closeCursor, and DML completion verified\n";
} finally {
    $remote->stop();
}
?>
--EXPECT--
lazy execution, preserved stream, closeCursor, and DML completion verified
