--TEST--
pdo_duckdb: Appender rejects recursive or unsupported VARIANT input without poisoning buffered rows
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->exec('CREATE TABLE variants (id INTEGER, v VARIANT)');
$app = $db->duckdbAppender('variants');
$app->appendRow(1, ['saved' => true]);
$array = [];
$array['self'] =& $array;
$object = new stdClass;
$object->self = $object;
$mixed = new stdClass;
$mixed->items = [&$mixed];
$resource = fopen('php://memory', 'r+');
$unsupported = new class {
    public function __toString(): string { throw new Exception('must not invoke callbacks'); }
    public function __destruct() {}
};
$deep = 'leaf';
for ($i = 0; $i < 130; $i++) $deep = [$deep];
foreach ([
    'array cycle' => $array,
    'object cycle' => $object,
    'mixed cycle' => $mixed,
    'resource' => ['resource' => $resource],
    'object' => ['object' => $unsupported],
    'invalid UTF-8' => ['bad' => "\xff"],
    'depth' => $deep,
] as $label => $value) {
    try {
        $app->appendRow(99, $value);
        echo "$label accepted\n";
    } catch (ValueError|TypeError $e) {
        echo "$label rejected\n";
    }
}
$shared = (object)['value' => 2];
$app->appendRow(2, [$shared, $shared]);
$app->close();
var_dump($db->query('SELECT id, v::JSON FROM variants ORDER BY id')->fetchAll(PDO::FETCH_NUM));
fclose($resource);
?>
--EXPECT--
array cycle rejected
object cycle rejected
mixed cycle rejected
resource rejected
object rejected
invalid UTF-8 rejected
depth rejected
array(2) {
  [0]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    string(14) "{"saved":true}"
  }
  [1]=>
  array(2) {
    [0]=>
    int(2)
    [1]=>
    string(25) "[{"value":2},{"value":2}]"
  }
}
