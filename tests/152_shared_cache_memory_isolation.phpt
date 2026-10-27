--TEST--
pdo_duckdb: independent unnamed in-memory connections remain isolated
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
foreach (['duckdb::memory:', 'duckdb:'] as $dsn) {
    $a = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $b = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $marker = bin2hex(random_bytes(16));
    $a->exec('CREATE TABLE t (marker VARCHAR)');
    $a->exec('INSERT INTO t VALUES (' . $a->quote($marker) . ')');
    echo 'other_catalog_empty=', (int) $b->query("SELECT count(*) FROM information_schema.tables WHERE table_name='t'")->fetchColumn() === 0 ? 'yes' : 'BAD', "\n";
    $b->exec('CREATE TABLE t (id INTEGER)');
    $b->exec('INSERT INTO t VALUES (22)');
    echo 'a_retained=', $a->query('SELECT marker FROM t')->fetchColumn() === $marker ? 'yes' : 'BAD', "\n";
    $a = null;
    echo 'b_survives=', $b->query('SELECT id FROM t')->fetchColumn(), "\n";
    $b = null;
}
$a = new PDO('duckdb::memory:;threads=1');
$b = new PDO('duckdb::memory:;threads=2');
echo 'independent_config=', $a->query("SELECT current_setting('threads')")->fetchColumn(), ',', $b->query("SELECT current_setting('threads')")->fetchColumn(), "\n";
?>
--EXPECT--
other_catalog_empty=yes
a_retained=yes
b_survives=22
other_catalog_empty=yes
a_retained=yes
b_survives=22
independent_config=1,2
