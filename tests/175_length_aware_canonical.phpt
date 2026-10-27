--TEST--
pdo_duckdb: default nested rendering matches DuckDB without truncating NUL bytes
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$expressions = [
 "['a'||chr(0)||'b']",
 "struct_pack(s := 'a'||chr(0)||'b')",
 "MAP(['a'||chr(0)||'b'], ['c'||chr(0)||'d'])",
 "['a,b','x']::VARCHAR[2]",
 "[TIME '12:34:56']::TIME[1]",
 "[TIMESTAMP '2024-02-29 12:34:56']::TIMESTAMP[1]",
 "union_value(s := 'a'||chr(0)||'b')",
 "(1, 'a'||chr(0)||'b')",
 "['a'||chr(0)||'b,c']",
 "['a,b'||chr(0)||'c']",
 "['a'||chr(0)||chr(39)||'b']",
 "['a'||chr(0)||chr(34)||'b']",
 "['a'||chr(0)||chr(92)||'b']",
 "[' '||chr(0)||'a', 'a'||chr(0)||' ']",
 "[','||chr(0)||chr(39), chr(92)||chr(0)||',']::VARCHAR[2]",
 "struct_pack(v := ['a,b'||chr(0)||chr(39)])",
 "MAP(['a'||chr(0)||',b'], [chr(39)||chr(0)||chr(92)])",
];
foreach ($expressions as $index => $expression) {
    [$got,$want] = $db->query("SELECT $expression, CAST($expression AS VARCHAR)")->fetch(PDO::FETCH_NUM);
    if ($got !== $want) throw new Exception("case $index: " . bin2hex($got) . ' != ' . bin2hex($want));
}
echo "canonical differential OK\n";
if ($db->query("SELECT ['a'||chr(0)||'b']")->fetchColumn() !== "[a\0b]") throw new Exception('NUL truncated');
echo "embedded NUL retained OK\n";
?>
--EXPECT--
canonical differential OK
embedded NUL retained OK
