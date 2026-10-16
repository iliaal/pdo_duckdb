--TEST--
pdo_duckdb: transaction batches accept high-byte dollar-quote tags and keep body semicolons intact
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

$tags = [
    'ff' => "\xff\xff",
    'fe' => "\xfe",
    'emoji' => "\xf0\x9f\x98\x80",
    'mixed' => "a\xffb",
    'empty' => '',
];

foreach ($tags as $label => $name) {
    $tag = '$' . $name . '$';
    $inner = '$inner$';
    $body = "x;BEGIN;ROLLBACK;{$inner}y;z{$inner} end";
    echo "$label plain=", bin2hex($db->query("SELECT {$tag}{$body}{$tag}")->fetchColumn()), "\n";
    echo "$label batch=", $db->exec("BEGIN;SELECT {$tag}{$body}{$tag};SELECT {$tag};{$tag};ROLLBACK"), "\n";
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