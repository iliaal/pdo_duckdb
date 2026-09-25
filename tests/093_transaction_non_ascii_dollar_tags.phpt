--TEST--
pdo_duckdb: transaction batches accept DuckDB non-ASCII dollar-quote tags
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
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
echo 'raw=', $db->exec("BEGIN;SELECT {$rawTag};{$rawTag};ROLLBACK"), "\n";
echo 'raw state=', $db->inTransaction() ? 'active' : 'idle', "\n";
echo 'single=', $db->query("SELECT {$tag}value;body{$tag}")->fetchColumn(), "\n";
?>
--EXPECT--
separated=0
separated state=idle
body=0
body state=idle
numeric=0
numeric state=idle
raw=0
raw state=idle
single=value;body
