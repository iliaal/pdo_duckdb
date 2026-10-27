--TEST--
pdo_duckdb: remote prepare never writes and execute preserves values, counts, and transactions
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
$server = $remote->server;
$client = $remote->client;
try {
    foreach (['attached', 'connected'] as $mode) {
        $server->exec('DELETE FROM remote_t');
        if ($mode === 'connected') {
            $client->exec('CONNECT q');
        }
        $client->exec("CREATE OR REPLACE MACRO from_hex(x) AS 'shadow'; CREATE OR REPLACE MACRO decode(x) AS 'shadow'");
        $table = $mode === 'attached' ? 'q.remote_t' : 'remote_t';
        $literal = $client->prepare("INSERT INTO $table VALUES (0, 'literal', 'literal', NULL)");
        if ($server->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 0) {
            throw new RuntimeException("$mode: prepare executed the literal INSERT");
        }
        $literal->execute();
        if ($literal->rowCount() !== 1 || $server->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 1) {
            throw new RuntimeException("$mode: execute did not insert exactly one literal row");
        }
        $insert = $client->prepare("INSERT INTO $table VALUES (:id, :text, :text, :blob)");
        if ($server->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 1) {
            throw new RuntimeException("$mode: preparing bound INSERT changed rows");
        }
        $id = 1;
        $text = "a\0b'\\? :ignored $" . bin2hex(random_bytes(8));
        $binary = "\x00\xff\x80'\\binary";
        $stream = fopen('php://memory', 'w+');
        fwrite($stream, $binary);
        rewind($stream);
        $insert->bindParam('id', $id, PDO::PARAM_INT);
        $insert->bindParam('text', $text, PDO::PARAM_STR);
        $insert->bindValue('blob', $stream, PDO::PARAM_LOB);
        $expected = [[0, 'literal', 'literal', null]];
        for ($i = 0; $i < 2; $i++) {
            $insert->execute();
            if ($insert->rowCount() !== 1 || ftell($stream) !== 0) {
                throw new RuntimeException("$mode: wrong affected rows or consumed replayable LOB");
            }
            $expected[] = [$id, $text, $text, $binary];
            $id++;
            $text .= "changed\0";
        }
        if ($server->query('SELECT * FROM remote_t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $expected) {
            throw new RuntimeException("$mode: repeated parameters, references, or binary bytes changed");
        }
        $client->exec('SET VARIABLE raw_session=7');
        $raw = $client->prepare('SELECT $raw_session + $RAW_SESSION');
        $raw->bindValue(1, 3, PDO::PARAM_INT);
        $raw->execute();
        if ($raw->fetchAll(PDO::FETCH_COLUMN) !== [6]) {
            throw new RuntimeException("$mode: raw native parameter binding changed");
        }
        $raw->execute([]);
        if ($raw->fetchAll(PDO::FETCH_COLUMN) !== [14]) {
            throw new RuntimeException("$mode: clearing raw bindings retained a stale value");
        }
        $numbered = $client->prepare('SELECT $1 + ?');
        $numbered->bindValue(1, 4, PDO::PARAM_INT);
        $numbered->execute();
        if ($numbered->fetchAll(PDO::FETCH_COLUMN) !== [8]) {
            throw new RuntimeException("$mode: raw numbered parameter binding changed");
        }
        $combined = $client->prepare('SELECT $2, ?');
        $combined->bindValue(1, 4, PDO::PARAM_INT);
        $combined->bindValue(2, 12, PDO::PARAM_INT);
        $combined->execute();
        if ($combined->fetchAll(PDO::FETCH_NUM) !== [[12, 4]]) {
            throw new RuntimeException("$mode: raw numbered parameter collided with a PDO binding");
        }
        $adjacent = $client->prepare('SELECT $1abc, $1_a, $1é');
        $adjacent->execute([3]);
        if ($adjacent->fetchAll(PDO::FETCH_ASSOC) !== [['abc' => '3', '_a' => '3', 'é' => '3']]) {
            throw new RuntimeException("$mode: adjacent identifier changed a native numbered placeholder");
        }
        $client->exec('SET VARIABLE "a""b"=19');
        foreach (['$"raw_session"' => 7, '$ raw_session' => 7,
                  '$ /* nested /* comment */ end */ raw_session' => 7,
                  '$`raw_session`' => 7, '$"a""b"' => 19] as $token => $expectedVariable) {
            $quoted = $client->prepare("SELECT $token, $token");
            $quoted->execute([3]);
            if ($quoted->fetchAll(PDO::FETCH_NUM) !== [['3', '3']]) {
                throw new RuntimeException("$mode: quoted native name lost its repeated binding");
            }
            $quoted->execute([]);
            if ($quoted->fetchAll(PDO::FETCH_NUM) !== [[$expectedVariable, $expectedVariable]]) {
                throw new RuntimeException("$mode: quoted native name retained a stale binding");
            }
            $mixedQuoted = $client->prepare("SELECT $token, CAST(? AS INTEGER), $token");
            $mixedQuoted->execute([3]);
            if ($mixedQuoted->fetchAll(PDO::FETCH_NUM) !== [[$expectedVariable, 3, $expectedVariable]]) {
                throw new RuntimeException("$mode: quoted session variable collided with a PDO binding");
            }
        }
        $unicode = $client->prepare("SELECT $\u{00a0}raw_session, \$raw_session");
        $unicode->execute([3]);
        if ($unicode->fetchAll(PDO::FETCH_NUM) !== [['3', 7]]) {
            throw new RuntimeException("$mode: native identifier whitespace changed its name");
        }
        $underscores = $client->prepare('SELECT $0_1, $01, $1');
        $underscores->execute([3]);
        if ($underscores->fetchAll(PDO::FETCH_NUM) !== [['3', '3', '3']]) {
            throw new RuntimeException("$mode: numeric underscore aliases lost their binding");
        }
        $ten = $client->prepare('SELECT $1_0, $1, $2, $3, $4, $5, $6, $7, $8, $9');
        $ten->execute(range(1, 10));
        if ($ten->fetchAll(PDO::FETCH_NUM) !== [['10', '1', '2', '3', '4', '5', '6', '7', '8', '9']]) {
            throw new RuntimeException("$mode: numeric underscore changed the parameter index");
        }
        try {
            $client->prepare('SELECT $1_0')->execute([3]);
            throw new RuntimeException("$mode: parameter one silently replaced parameter ten");
        } catch (PDOException $e) {
            if (!str_contains(strtolower($e->getMessage()), 'param')) {
                throw $e;
            }
        }
        $mixed = $client->prepare('SELECT $raw_session + :raw_session + :raw_session');
        $mixed->bindValue('raw_session', 5, PDO::PARAM_INT);
        $mixed->execute();
        if ($mixed->fetchAll(PDO::FETCH_COLUMN) !== [17]) {
            throw new RuntimeException("$mode: native variable and repeated PDO binding collided");
        }
        foreach ([[], ['extra' => 5]] as $bindings) {
            try {
                $mixed->execute($bindings);
                throw new RuntimeException("$mode: missing or excess PDO binding was accepted");
            } catch (PDOException $e) {
                if (!str_contains(strtolower($e->getMessage()), 'parameter')) {
                    throw $e;
                }
            }
        }
        $repeatedBlob = $client->prepare('SELECT :blob AS first, :blob AS second');
        $repeatedBlob->bindValue('blob', $stream, PDO::PARAM_LOB);
        $repeatedBlob->execute();
        if ($repeatedBlob->fetchAll(PDO::FETCH_NUM) !== [[$binary, $binary]] || ftell($stream) !== 0) {
            throw new RuntimeException("$mode: repeated BLOB parameter did not reuse its exact bytes");
        }
        $typed = $client->prepare('SELECT :integer AS i, :boolean AS b, :null AS n');
        $typed->bindValue('integer', -7, PDO::PARAM_INT);
        $typed->bindValue('boolean', true, PDO::PARAM_BOOL);
        $typed->bindValue('null', null, PDO::PARAM_NULL);
        $typed->execute();
        if ($typed->getColumnMeta(0)['native_type'] !== 'BIGINT'
            || $typed->getColumnMeta(1)['native_type'] !== 'BOOLEAN'
            || $typed->fetchAll(PDO::FETCH_NUM) !== [[-7, 1, null]]) {
            throw new RuntimeException("$mode: typed parameters lost their values or SQL types");
        }
        $select = $client->prepare("SELECT id,s,repeated,b FROM $table WHERE id >= ? ORDER BY id");
        $select->execute([1]);
        if ($select->rowCount() !== 0 || $select->columnCount() !== 4
            || $select->getColumnMeta(3)['native_type'] !== 'BLOB'
            || $select->fetchAll(PDO::FETCH_NUM) !== array_slice($expected, 1)) {
            throw new RuntimeException("$mode: prepared SELECT lost metadata or rows");
        }
        $client->beginTransaction();
        $insert->execute();
        if ($server->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 3) {
            throw new RuntimeException("$mode: uncommitted row became visible");
        }
        $client->rollBack();
        if ($server->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 3) {
            throw new RuntimeException("$mode: rollback retained a row");
        }
        $client->beginTransaction();
        $insert->execute();
        $client->commit();
        $expected[] = [$id, $text, $text, $binary];
        if ($server->query('SELECT * FROM remote_t ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== $expected) {
            throw new RuntimeException("$mode: commit lost or duplicated a row");
        }
        $counts = $mode === 'attached' ? 'q.remote_counts' : 'remote_counts';
        $server->exec('DELETE FROM remote_counts');
        $server->exec('INSERT INTO remote_counts VALUES (1,10), (2,20)');
        $update = $client->prepare("UPDATE $counts SET v=CAST(? AS INTEGER) WHERE id=1");
        if ($server->query('SELECT v FROM remote_counts WHERE id=1')->fetchColumn() !== 10) {
            throw new RuntimeException("$mode: preparing UPDATE changed data");
        }
        $update->execute([11]);
        if ($update->rowCount() !== 1 || $server->query('SELECT v FROM remote_counts WHERE id=1')->fetchColumn() !== 11) {
            throw new RuntimeException("$mode: UPDATE lost its affected count or value");
        }
        $delete = $client->prepare("DELETE FROM $counts WHERE id=CAST(? AS INTEGER)");
        $delete->execute([2]);
        if ($delete->rowCount() !== 1 || $server->query('SELECT count(*) FROM remote_counts')->fetchColumn() !== 1) {
            throw new RuntimeException("$mode: DELETE lost its affected count or row change");
        }
        $merge = $client->prepare("MERGE INTO $counts AS dst USING (SELECT CAST(? AS INTEGER) AS id, 30 AS v) AS src "
            . 'ON dst.id=src.id WHEN MATCHED THEN UPDATE SET v=src.v WHEN NOT MATCHED THEN INSERT VALUES (src.id,src.v)');
        $merge->execute([3]);
        if ($merge->rowCount() !== 1 || $server->query('SELECT * FROM remote_counts ORDER BY id')->fetchAll(PDO::FETCH_NUM) !== [[1,11],[3,30]]) {
            throw new RuntimeException("$mode: MERGE lost its affected count or inserted row");
        }
        if ($client->exec("INSERT INTO $counts VALUES (4,40),(5,50)") !== 2
            || $client->exec("UPDATE $counts SET v=v+1 WHERE id>=4") !== 2
            || $client->exec("DELETE FROM $counts WHERE id=5") !== 1
            || $client->exec("MERGE INTO $counts AS dst USING (SELECT 1 AS id,99 AS v) AS src "
                . 'ON dst.id=src.id WHEN MATCHED THEN UPDATE SET v=src.v') !== 1) {
            throw new RuntimeException("$mode: exec lost INSERT, UPDATE, DELETE, or MERGE affected rows");
        }
        if ($client->exec("INSERT INTO $counts VALUES (6,60),(7,70); UPDATE $counts SET v=61 WHERE id=6") !== 1
            || $client->exec('/* outer ; /* inner ; */ */ SELECT $$semi; :x ?$$; '
                . "INSERT INTO $counts VALUES (8,80),(9,90)") !== 0
            || $client->exec("INSERT INTO $counts VALUES (10,100); SELECT 42") !== 0) {
            throw new RuntimeException("$mode: exec batch counts or SQL boundaries changed");
        }
        if ($client->exec("BEGIN; INSERT INTO $counts VALUES (11,110),(12,120)") !== 2 || !$client->inTransaction()) {
            throw new RuntimeException("$mode: exec batch did not leave an active transaction");
        }
        $client->rollBack();
        if ($client->exec("BEGIN; INSERT INTO $counts VALUES (13,130); COMMIT") !== 0 || $client->inTransaction()) {
            throw new RuntimeException("$mode: exec batch did not commit and close the transaction");
        }
        try {
            $client->exec("BEGIN; INSERT INTO $counts VALUES (14,140); COMMIT; INSERT INTO absent_table VALUES (1)");
            throw new RuntimeException("$mode: invalid final statement unexpectedly succeeded");
        } catch (PDOException $e) {
            if ($client->inTransaction() || !str_contains($e->getMessage(), 'absent_table')) {
                throw new RuntimeException("$mode: a committed prefix lost its transaction state", 0, $e);
            }
        }
        try {
            $client->exec("INSERT INTO $counts VALUES (16,160); SELEC invalid");
            throw new RuntimeException("$mode: invalid final syntax unexpectedly succeeded");
        } catch (PDOException $e) {
            if (!str_contains($e->getMessage(), 'Parser Error')) {
                throw $e;
            }
        }
        $client->beginTransaction();
        try {
            $client->exec("INSERT INTO $counts VALUES (17,170); SELEC invalid");
            throw new RuntimeException("$mode: invalid syntax in a transaction unexpectedly succeeded");
        } catch (PDOException $e) {
            if (!str_contains($e->getMessage(), 'Parser Error') || !$client->inTransaction()) {
                throw $e;
            }
        }
        $client->commit();
        if ($server->query('SELECT * FROM remote_counts ORDER BY id')->fetchAll(PDO::FETCH_NUM)
            !== [[1,99],[3,30],[4,41],[6,61],[7,70],[8,80],[9,90],[10,100],[13,130],[14,140],[16,160]]) {
            throw new RuntimeException("$mode: exec batch lost, duplicated, or retained rolled-back rows");
        }
        $client->beginTransaction();
        $client->exec("INSERT INTO $counts VALUES (18,180)");
        try {
            $client->prepare("INSERT INTO $counts VALUES (19,190) RETURNING");
            throw new RuntimeException("$mode: malformed prepare unexpectedly succeeded");
        } catch (PDOException $e) {
            if (!str_contains($e->getMessage(), 'Parser Error') || !$client->inTransaction()) {
                throw $e;
            }
        }
        if ($server->query('SELECT id FROM remote_counts WHERE id>=18')->fetchAll(PDO::FETCH_COLUMN) !== []) {
            throw new RuntimeException("$mode: rejected prepare changed committed server data");
        }
        if ($mode === 'connected') {
            if ($client->query("SELECT id FROM $counts WHERE id>=18")->fetchAll(PDO::FETCH_COLUMN) !== [18]) {
                throw new RuntimeException('Client-side syntax validation changed the connected transaction');
            }
            $missing = $client->prepare("INSERT INTO $counts VALUES (:id,:value)");
            try {
                $missing->execute(['id' => 21]);
                throw new RuntimeException('Missing binding unexpectedly executed remotely');
            } catch (PDOException $e) {
                if (!str_contains(strtolower($e->getMessage()), 'parameter')) {
                    throw $e;
                }
            }
            if ($client->query("SELECT id FROM $counts WHERE id>=18")->fetchAll(PDO::FETCH_COLUMN) !== [18]) {
                throw new RuntimeException('Client-side binding validation changed the connected transaction');
            }
            $client->exec("INSERT INTO $counts VALUES (20,200)");
        }
        $client->commit();
        if ($server->query('SELECT id FROM remote_counts WHERE id>=18 ORDER BY id')->fetchAll(PDO::FETCH_COLUMN)
            !== ($mode === 'connected' ? [18,20] : [])) {
            throw new RuntimeException("$mode: prepare validation violated the client/server transaction contract");
        }
        $client->beginTransaction();
        $client->exec("INSERT INTO $counts VALUES (22,220)");
        $conversion = $client->prepare("INSERT INTO $counts VALUES (23,CAST(? AS INTEGER))");
        try {
            $conversion->execute(['bad integer']);
            throw new RuntimeException("$mode: invalid conversion unexpectedly executed");
        } catch (PDOException $e) {
            if (!str_contains($e->getMessage(), 'Conversion Error') || !$client->inTransaction()) {
                throw $e;
            }
        }
        $client->commit();
        if ($server->query('SELECT id FROM remote_counts WHERE id>=22')->fetchAll(PDO::FETCH_COLUMN) !== []) {
            throw new RuntimeException("$mode: server execution error failed to invalidate the transaction");
        }
        fclose($stream);
        echo "$mode: deferred writes, exact values, SELECT, and transactions verified\n";
    }
    $client->exec('DISCONNECT');
    if ($client->query('SELECT 42')->fetchColumn() !== 42) {
        throw new RuntimeException('Local query failed after DISCONNECT');
    }
    $transition = duckdb_remote_connection();
    $transition->exec('CREATE TABLE remote_t (id INTEGER, s VARCHAR, repeated VARCHAR, b BLOB)');
    $beforeConnect = $transition->prepare('INSERT INTO remote_t VALUES (99, ?, NULL, NULL)');
    $transition->exec('ATTACH ' . $transition->quote($remote->endpoint)
        . ' AS q (TOKEN ' . $transition->quote($remote->token) . ', DISABLE_SSL true); CONNECT q');
    $beforeConnect->execute(["remote\0value"]);
    if ($beforeConnect->rowCount() !== 1
        || $server->query('SELECT s FROM remote_t WHERE id=99')->fetchAll(PDO::FETCH_COLUMN) !== ["remote\0value"]) {
        throw new RuntimeException('Statement prepared locally did not follow CONNECT');
    }
    $transition->exec('DISCONNECT');
    if ($transition->query('SELECT count(*) FROM remote_t')->fetchColumn() !== 0) {
        throw new RuntimeException('Remote execution wrote through the original local prepared handle');
    }
    $beforeConnect->execute(["local\0value"]);
    if ($beforeConnect->rowCount() !== 1
        || $transition->query('SELECT s FROM remote_t')->fetchAll(PDO::FETCH_COLUMN) !== ["local\0value"]
        || $server->query('SELECT s FROM remote_t WHERE id=99')->fetchAll(PDO::FETCH_COLUMN) !== ["remote\0value"]) {
        throw new RuntimeException('Statement did not follow DISCONNECT back to its local database');
    }
} finally {
    $remote->stop();
}
?>
--EXPECT--
attached: deferred writes, exact values, SELECT, and transactions verified
connected: deferred writes, exact values, SELECT, and transactions verified
