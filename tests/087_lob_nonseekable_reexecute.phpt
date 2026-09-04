--TEST--
pdo_duckdb: non-seekable LOB stream binds from the current position on re-execute
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// A read-only stream wrapper with no seek support: the driver cannot rewind
// it, so re-executing binds from the consumed position without error.
class PdoNoSeek {
    public $context;
    private string $data = 'nonseekable-payload';
    private int $pos = 0;
    public function stream_open($path, $mode, $options, &$opened_path) { $this->pos = 0; return true; }
    public function stream_read($count) {
        $chunk = substr($this->data, $this->pos, $count);
        $this->pos += strlen($chunk);
        return $chunk;
    }
    public function stream_eof() { return $this->pos >= strlen($this->data); }
    public function stream_stat() { return []; }
}
stream_wrapper_register('pdonoseek', PdoNoSeek::class);

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (b BLOB)');

$stream = fopen('pdonoseek://x', 'r');
$st = $db->prepare('INSERT INTO t VALUES (?)');
$st->bindValue(1, $stream, PDO::PARAM_LOB);
$st->execute();

// The first execute round-trips the full bytes; re-executing binds the
// exhausted remainder (empty) and touches nothing else.
$st->execute();
$rows = $db->query('SELECT b, octet_length(b) AS l FROM t ORDER BY l')->fetchAll(PDO::FETCH_ASSOC);
echo 'rows=', count($rows), "\n";
var_dump($rows[0]['b']);
var_dump($rows[1]['b']);
?>
--EXPECT--
rows=2
string(0) ""
string(19) "nonseekable-payload"
