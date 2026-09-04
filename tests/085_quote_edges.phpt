--TEST--
pdo_duckdb: PDO::quote edge inputs and sticky-error clearing
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);

// A NUL failure records a sticky general driver error ...
var_dump($db->quote("x\0y"));
$info = $db->errorInfo();
echo "nul_sqlstate={$info[0]}\n";
echo 'nul_driver=' . var_export($info[1], true) . "\n";
echo 'nul_msg=' . (isset($info[2]) && str_contains($info[2], 'null bytes') ? "yes\n" : "no\n");

// ... cleared by the next successful quote: PDO resets the SQLSTATE at quote
// entry and the driver drops its payload on success, leaving exactly 00000.
var_dump($db->quote("O'Reilly"));
$info = $db->errorInfo();
echo 'entries=' . count($info) . "\n";
echo 'clean=', (!isset($info[1]) && !isset($info[2]) ? "yes\n" : "no\n");

// Edge inputs: empty string, PARAM_INT (ignored: the value is always quoted),
// binary without NUL, and a quoted round-trip through the engine.
var_dump($db->quote(''));
var_dump($db->quote(123, PDO::PARAM_INT));
echo bin2hex($db->quote("\xff\xfe'b")), "\n";
var_dump($db->query('SELECT ' . $db->quote("a'b"))->fetchColumn());
?>
--EXPECT--
bool(false)
nul_sqlstate=HY000
nul_driver=1
nul_msg=yes
string(11) "'O''Reilly'"
entries=3
clean=yes
string(2) "''"
string(5) "'123'"
27fffe27276227
string(3) "a'b"
