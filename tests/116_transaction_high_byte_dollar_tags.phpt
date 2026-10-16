--TEST--
pdo_duckdb: transaction batches accept high-byte dollar-quote tags and keep body semicolons intact
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

$tags = [
    'ff' => "\xff\xff",
    'fe' => "\xfe",
    'emoji' => "\xf0\x9f\x98\x80",
    'mixed' => "a\xffb",
    'empty' => '',
];

$wantHex = '783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64';

// DuckDB 2.0 rejects non-UTF-8 query text up front; each rejection must be the
// exact parser error, leave no transaction open, and leave the handle usable.
function expect_utf8_rejection(PDO $db, callable $fn): bool
{
    try {
        $fn();
        echo "BAD: invalid UTF-8 accepted\n";
        return false;
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'Parser Error: Invalid UTF-8 in query')) {
            echo 'BAD: ', $e->getMessage(), "\n";
            return false;
        }
    }
    if ($db->inTransaction()) {
        echo "BAD: transaction left open\n";
        return false;
    }
    if ($db->query('SELECT 42')->fetchColumn() != 42) {
        echo "BAD: handle unusable after rejection\n";
        return false;
    }
    return true;
}

foreach ($tags as $label => $name) {
    $tag = '$' . $name . '$';
    $inner = '$inner$';
    $body = "x;BEGIN;ROLLBACK;{$inner}y;z{$inner} end";
    $plainSql = "SELECT {$tag}{$body}{$tag}";
    $batchSql = "BEGIN;SELECT {$tag}{$body}{$tag};SELECT {$tag};{$tag};ROLLBACK";
    $invalid = preg_match('//u', $name) !== 1;
    if ($v2 && $invalid) {
        echo "$label plain=", expect_utf8_rejection($db, fn() => $db->query($plainSql)) ? $wantHex : 'BAD', "\n";
        echo "$label batch=", expect_utf8_rejection($db, fn() => $db->exec($batchSql)) ? '0' : 'BAD', "\n";
    } else {
        echo "$label plain=", bin2hex($db->query($plainSql)->fetchColumn()), "\n";
        echo "$label batch=", $db->exec($batchSql), "\n";
    }
    echo "$label state=", $db->inTransaction() ? 'active' : 'idle', "\n";
}

$bad = '$1$';
try {
    $db->query("SELECT {$bad}x{$bad}");
    echo "control plain=accepted\n";
} catch (PDOException $e) {
    echo 'control plain=', str_contains($e->getMessage(), 'Parser Error') ? 'parser' : 'other', "\n";
}
try {
    $db->exec("BEGIN;SELECT {$bad}x;y{$bad};ROLLBACK");
    echo "control batch=accepted\n";
} catch (PDOException $e) {
    echo 'control batch=', str_contains($e->getMessage(), 'Parser Error') ? 'parser' : 'other', "\n";
}
echo 'control state=', $db->inTransaction() ? 'active' : 'idle', "\n";
?>
--EXPECT--
ff plain=783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64
ff batch=0
ff state=idle
fe plain=783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64
fe batch=0
fe state=idle
emoji plain=783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64
emoji batch=0
emoji state=idle
mixed plain=783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64
mixed batch=0
mixed state=idle
empty plain=783b424547494e3b524f4c4c4241434b3b24696e6e657224793b7a24696e6e65722420656e64
empty batch=0
empty state=idle
control plain=parser
control batch=parser
control state=idle