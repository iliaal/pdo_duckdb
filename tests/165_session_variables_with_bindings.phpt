--TEST--
pdo_duckdb: session variables coexist with positional and repeated named PDO parameters
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET VARIABLE x=7');
$db->exec("CREATE MACRO from_hex(x) AS 'shadow'; CREATE MACRO decode(x) AS 'shadow'");
if (!$v2) {
    try {
        $db->query('SELECT $x')->fetchColumn();
        throw new RuntimeException('Legacy engine unexpectedly resolved session-variable shorthand');
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'Values were not provided')) {
            throw $e;
        }
    }
}
$variable = $v2 ? '$x' : "getvariable('x')";
foreach ([false, true] as $unbuffered) {
    $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, $unbuffered);
    $db->exec('SET VARIABLE x=7');
    $positional = $db->prepare("SELECT $variable + CAST(? AS INTEGER)");
    $positional->execute([3]);
    if ($positional->fetchAll(PDO::FETCH_COLUMN) !== [10] || $positional->rowCount() !== 0) {
        throw new RuntimeException('Positional binding changed the session variable');
    }
    $named = $db->prepare("SELECT $variable + CAST(:x AS INTEGER) + CAST(:x AS INTEGER)");
    $bound = 3;
    $named->bindParam('x', $bound, PDO::PARAM_INT);
    $named->execute();
    if ($named->fetchAll(PDO::FETCH_COLUMN) !== [13]) {
        throw new RuntimeException('Repeated binding did not remain distinct from the session variable');
    }
    $db->exec('SET VARIABLE x=11');
    $bound = 5;
    $named->execute();
    if ($named->fetchAll(PDO::FETCH_COLUMN) !== [21]) {
        throw new RuntimeException('Re-execution reused a stale variable or reference binding');
    }
    if ($db->query("SELECT $variable")->fetchAll(PDO::FETCH_COLUMN) !== [11]) {
        throw new RuntimeException('Binding overwrote the session variable');
    }
    $negative = $db->prepare("SELECT $variable-:negative");
    $negative->bindValue('negative', -2, PDO::PARAM_INT);
    $negative->execute();
    if ($negative->fetchAll(PDO::FETCH_COLUMN) !== [13]) {
        throw new RuntimeException('Expanded negative value merged with the SQL minus token');
    }
    $nul = $db->prepare("SELECT $variable, :value, :value");
    $nul->execute(['value' => "a\0b"]);
    if ($nul->fetchAll(PDO::FETCH_NUM) !== [[11, "a\0b", "a\0b"]]) {
        throw new RuntimeException('User macros changed a bound NUL string');
    }
    $raw = $db->prepare('SELECT $x + $X');
    $raw->bindValue(1, 3, PDO::PARAM_INT);
    $raw->execute();
    if ($raw->fetchAll(PDO::FETCH_COLUMN) !== [6]) {
        throw new RuntimeException('Native named parameter lost positional binding compatibility');
    }
    $numbered = $db->prepare('SELECT $1 + ?');
    $numbered->bindValue(1, 4, PDO::PARAM_INT);
    $numbered->execute();
    if ($numbered->fetchAll(PDO::FETCH_COLUMN) !== [8]) {
        throw new RuntimeException('Native numbered parameter lost positional binding compatibility');
    }
    $combined = $db->prepare('SELECT $2, ?');
    $combined->bindValue(1, 4, PDO::PARAM_INT);
    $combined->bindValue(2, 12, PDO::PARAM_INT);
    $combined->execute();
    if ($combined->fetchAll(PDO::FETCH_NUM) !== [[12, 4]]) {
        throw new RuntimeException('Native numbered parameter collided with a PDO positional binding');
    }
    $adjacent = $db->prepare('SELECT $1abc, $1_a, $1é');
    $adjacent->execute([3]);
    if ($adjacent->fetchAll(PDO::FETCH_ASSOC) !== [['abc' => '3', '_a' => '3', 'é' => '3']]) {
        throw new RuntimeException('Adjacent identifier changed a native numbered placeholder');
    }
    foreach (['$"x"', '$ x', '$ /* nested /* comment */ end */ x', '$"a""b"'] as $token) {
        $quoted = $db->prepare("SELECT $token, $token");
        $quoted->execute([3]);
        if ($quoted->fetchAll(PDO::FETCH_NUM) !== [['3', '3']]) {
            throw new RuntimeException('Quoted or separated native parameter lost its repeated binding');
        }
    }
    if ($v2) {
        $db->exec('SET VARIABLE "a""b"=19');
        foreach (['$"x"' => 11, '$ x' => 11, '$ /* comment */ x' => 11,
                  '$`x`' => 11, '$"a""b"' => 19] as $token => $expected) {
            $quoted = $db->prepare("SELECT $token, CAST(? AS INTEGER), $token");
            $quoted->execute([3]);
            if ($quoted->fetchAll(PDO::FETCH_NUM) !== [[$expected, 3, $expected]]) {
                throw new RuntimeException('Quoted or separated session variable collided with a PDO binding');
            }
        }
        $underscores = $db->prepare('SELECT $0_1, $01, $1');
        $underscores->execute([3]);
        if ($underscores->fetchAll(PDO::FETCH_NUM) !== [['3', '3', '3']]) {
            throw new RuntimeException('Numeric underscore aliases did not share one binding');
        }
        $ten = $db->prepare('SELECT $1_0, $1, $2, $3, $4, $5, $6, $7, $8, $9');
        $ten->execute(range(1, 10));
        if ($ten->fetchAll(PDO::FETCH_NUM) !== [['10', '1', '2', '3', '4', '5', '6', '7', '8', '9']]) {
            throw new RuntimeException('Numeric underscore changed the native parameter index');
        }
    } else {
        $underscores = $db->prepare('SELECT $1_0, $1e0');
        $underscores->execute([3]);
        if ($underscores->fetchAll(PDO::FETCH_ASSOC) !== [['$1' => '3', 'e0' => '3']]) {
            throw new RuntimeException('Legacy numeric parameter interpretation changed');
        }
        foreach ([
            ['SELECT $0, $0, $1', [3, 4], ['3', '4', '3']],
            ['SELECT ?, $0', [3, 4], ['3', '4']],
            ['SELECT $0, ?', [3], ['3', '3']],
            ['SELECT ?, $0, ?', [3, 4], ['3', '4', '4']],
        ] as [$sql, $values, $expected]) {
            $anonymous = $db->prepare($sql);
            $anonymous->execute($values);
            if ($anonymous->fetchAll(PDO::FETCH_NUM) !== [$expected]) {
                throw new RuntimeException('Legacy anonymous parameter numbering changed');
            }
        }
    }
    echo $unbuffered ? 'streaming' : 'buffered', ": variables and bindings verified\n";
}
$batch = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$batch->exec("CREATE TABLE t(v INTEGER); CREATE TABLE source(v VARCHAR); INSERT INTO source VALUES('bad')");
foreach ([
    ['BEGIN; INSERT INTO missing_table VALUES(1)', '42000', true],
    ['BEGIN; SELECT missing_column', '42000', true],
    ["BEGIN; INSERT INTO t VALUES('bad')", '42000', true],
    ['BEGIN; SELECT CAST(v AS INTEGER) FROM source', 'HY000', true],
    ['BEGIN; INSERT INTO t VALUES(1); SELEC invalid', '42000', false],
] as [$sql, $code, $active]) {
    try {
        $batch->exec($sql);
        throw new RuntimeException('Invalid transaction batch unexpectedly succeeded');
    } catch (PDOException $e) {
        if ($e->getCode() !== $code || $batch->inTransaction() !== $active) {
            throw new RuntimeException('Transaction batch changed its error phase or transaction state', 0, $e);
        }
    }
    if ($batch->inTransaction()) {
        $batch->rollBack();
    }
    if ($batch->query('SELECT v FROM t')->fetchAll(PDO::FETCH_COLUMN) !== []) {
        throw new RuntimeException('Invalid transaction batch retained a write');
    }
}
if ($v2) {
    foreach ([null, 'SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'] as $policy) {
        $options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
        if ($policy !== null) {
            $options[PDO::DUCKDB_ATTR_CONFIG] = ['default_transaction_invalidation_policy' => $policy];
        }
        $tx = new PDO('duckdb::memory:', null, null, $options);
        $tx->exec('CREATE TABLE t(v INTEGER); SET VARIABLE x=7');
        foreach (['SELECT 1 FROM', 'SELECT $x + :value FROM'] as $invalid) {
            $tx->exec('DELETE FROM t');
            $tx->beginTransaction();
            $tx->exec('INSERT INTO t VALUES (42)');
            try {
                $tx->prepare($invalid);
                throw new RuntimeException('Malformed SQL unexpectedly prepared');
            } catch (PDOException $e) {
                if (!str_contains($e->getMessage(), 'Parser Error')) {
                    throw $e;
                }
            }
            if (!$tx->inTransaction() || !$tx->commit()) {
                throw new RuntimeException('Malformed prepare changed transaction control state');
            }
            if ($tx->query('SELECT v FROM t')->fetchAll(PDO::FETCH_COLUMN) !== ($policy === null ? [] : [42])) {
                throw new RuntimeException('Deferred parse error bypassed the transaction invalidation policy');
            }
        }
    }
}
?>
--EXPECT--
buffered: variables and bindings verified
streaming: variables and bindings verified
