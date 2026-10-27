--TEST--
pdo_duckdb: native nested values survive buffered and streaming chunks, interleaving refusal, and cursor close
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
function check_native_row($value, int $index): void {
    $want = ['type'=>'STRUCT(i INTEGER, s VARCHAR[])','value'=>['i'=>$index,'s'=>["a\0b",(string)$index]]];
    if (get_debug_type($value) !== 'stdClass' || get_object_vars($value) !== $want) {
        throw new Exception("Bad native row $index");
    }
}
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
foreach ([false,true] as $unbuffered) {
    $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, $unbuffered);
    if ($db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED) !== $unbuffered) throw new Exception('Unbuffered option not applied');
    $st = $db->prepare("SELECT struct_pack(i := i::INTEGER, s := ['a'||chr(0)||'b', i::VARCHAR]) FROM range(4100) t(i)");
    $st->execute();
    $saved = [];
    for ($i=0; $i<4100; $i++) {
        $value = $st->fetchColumn();
        check_native_row($value, $i);
        if ($i % 2048 === 0) $saved[$i] = $value;
        if ($i === 0 && !$unbuffered) {
            if ($db->query('SELECT 99')->fetchColumn() !== 99) throw new Exception('Buffered interleave failed');
        } elseif ($i === 0 && $v2) {
            try {
                $db->query('SELECT 99');
                throw new Exception('An active unbuffered result accepted another statement');
            } catch (PDOException $e) {
                if (!str_contains($e->getMessage(), 'connection has an open result')) throw $e;
            }
        }
    }
    if ($st->fetchColumn() !== false) throw new Exception('Expected EOF');
    $st->execute();
    $st->closeCursor();
    unset($st);
    foreach ($saved as $i=>$value) {
        check_native_row($value, $i);
    }
    echo $unbuffered ? "streamed lifetime OK\n" : "buffered lifetime OK\n";
}
?>
--EXPECT--
buffered lifetime OK
streamed lifetime OK
