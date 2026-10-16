--TEST--
pdo_duckdb: transaction batches accept DuckDB non-ASCII dollar-quote tags
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$tag = "\x24\xc3\xa9\x24";
$numericTag = "\x24tag1\x24";

foreach ([
    'separated' => "BEGIN;SELECT {$tag};{$tag};ROLLBACK",
    'body' => "BEGIN;SELECT {$tag}BEGIN;ROLLBACK{$tag};ROLLBACK",
    'numeric' => "BEGIN;SELECT {$numericTag};{$numericTag};ROLLBACK",
] as $label => $sql) {
    echo "$label=", $db->exec($sql), "\n";
    echo "$label state=", $db->inTransaction() ? 'active' : 'idle', "\n";
}

$rawTag = "\x24\x80\x24";
if (pdo_duckdb_v2_headers()) {
    // DuckDB 2.0 rejects invalid UTF-8 in the query text before running any
    // statement of the batch, so no transaction may be left open.
    try {
        $db->exec("BEGIN;SELECT {$rawTag};{$rawTag};ROLLBACK");
        echo "raw=BAD: invalid UTF-8 accepted\n";
    } catch (PDOException $e) {
        echo 'raw=', str_contains($e->getMessage(), 'Parser Error: Invalid UTF-8 in query') ? 'ok' : 'BAD: ' . $e->getMessage(), "\n";
    }
    echo 'raw state=', $db->inTransaction() ? 'active' : 'idle', "\n";
    try {
        $db->exec('COMMIT');
        echo "BAD: DuckDB transaction left open\n";
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'no transaction is active')) {
            echo 'BAD: ', $e->getMessage(), "\n";
        }
    }
} else {
    $raw = $db->exec("BEGIN;SELECT {$rawTag};{$rawTag};ROLLBACK");
    echo 'raw=', $raw === 0 ? 'ok' : var_export($raw, true), "\n";
    echo 'raw state=', $db->inTransaction() ? 'active' : 'idle', "\n";
}
try {
    $db->exec('BEGIN;SELECT $1$;ROLLBACK');
    echo "digit_first=accepted\n";
} catch (PDOException $e) {
    echo 'digit_first=', str_contains($e->getMessage(), 'Parser Error') ? 'parser' : 'other', "\n";
}
echo 'single=', $db->query("SELECT {$tag}value;body{$tag}")->fetchColumn(), "\n";
?>
--EXPECT--
separated=0
separated state=idle
body=0
body state=idle
numeric=0
numeric state=idle
raw=ok
raw state=idle
digit_first=parser
single=value;body
