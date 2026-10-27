--TEST--
pdo_duckdb: native structures default, prepare snapshots, and stringification precedence
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function check($name, $got, $want) {
    if ($got !== $want) throw new Exception($name . ': ' . var_export($got, true));
    echo "$name OK\n";
}
function check_native($name, $got, $type, $value) {
    if (get_debug_type($got) !== 'stdClass') throw new Exception($name . ': expected stdClass');
    check($name, get_object_vars($got), ['type'=>$type,'value'=>$value]);
}
$attr = PDO::DUCKDB_ATTR_NATIVE_STRUCTURES;
$db = new PDO('duckdb::memory:');
check('default', $db->getAttribute($attr), false);
$old = $db->prepare('SELECT [1,2]');
$db->setAttribute($attr, true);
$new = $db->prepare('SELECT [1,2]');
$old->execute();
$new->execute();
check('old snapshot', $old->fetchColumn(), '[1, 2]');
check_native('new snapshot', $new->fetchColumn(), 'INTEGER[]', [1,2]);
check('statement get', $new->getAttribute($attr), true);
$new->setAttribute($attr, false);
$new->execute();
check('statement override', $new->fetchColumn(), '[1, 2]');
$forced = $db->prepare('SELECT [1,2]', [$attr => false]);
$forced->execute();
check('prepare override', $forced->fetchColumn(), '[1, 2]');
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, true);
check('stringify', $db->query('SELECT [1,2]')->fetchColumn(), '[1, 2]');
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, false);
$bound = $db->query('SELECT [1,2]');
$bound->bindColumn(1, $value, PDO::PARAM_STR);
$bound->fetch(PDO::FETCH_BOUND);
check('explicit scalar bind', $value, '[1, 2]');
$ctor = new PDO('duckdb::memory:', null, null, [$attr => true]);
check_native('constructor option', $ctor->query('SELECT [1]')->fetchColumn(), 'INTEGER[]', [1]);
check('structured SQL null', $ctor->query('SELECT NULL::INTEGER[]')->fetchColumn(), null);
?>
--EXPECT--
default OK
old snapshot OK
new snapshot OK
statement get OK
statement override OK
prepare override OK
stringify OK
explicit scalar bind OK
constructor option OK
structured SQL null OK
