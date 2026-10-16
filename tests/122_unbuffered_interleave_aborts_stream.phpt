--TEST--
pdo_duckdb: another statement aborts a partially read unbuffered result at its next chunk
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

function drain(PDOStatement $stmt, int &$seen): string
{
    try {
        while (($v = $stmt->fetchColumn()) !== false) {
            if ((int) $v !== $seen) {
                return "BAD: row $seen read back as $v";
            }
            $seen++;
        }
        return "completed";
    } catch (PDOException $e) {
        $aborted = str_contains($e->getMessage(), 'Attempting to execute an unsuccessful or closed')
            && $e->errorInfo[1] === 5;
        return $aborted ? "aborted" : "BAD: unexpected error " . $e->getMessage();
    }
}

$interleave = [
    'query' => fn() => $db->query('SELECT 99')->fetchColumn(),
    'exec' => fn() => $db->exec('CREATE TEMP TABLE IF NOT EXISTS scratch (i INTEGER)'),
    'prepare' => fn() => $db->prepare('SELECT 1'),
];

foreach ($interleave as $label => $other) {
    $first = $db->query('SELECT i FROM range(5000) t(i)');
    $seen = 0;
    $first->fetchColumn();
    $seen++;
    $other();
    // The rows already pulled in DuckDB's first 2048-row vector stay readable;
    // the next chunk pull reports the abort.
    echo "$label: ", drain($first, $seen), " after $seen rows\n";
    var_dump($first->fetchColumn());
    var_dump($first->closeCursor());
    $first->execute();
    $seen = 0;
    echo "$label rerun: ", drain($first, $seen), " $seen rows\n";
}

$small = $db->query('SELECT i FROM range(10) t(i)');
$small->fetchColumn();
$db->query('SELECT 99')->fetchColumn();
$seen = 1;
echo "single chunk: ", drain($small, $seen), " after $seen rows\n";

$alone = $db->query('SELECT i FROM range(5000) t(i)');
$seen = 0;
echo "no interleave: ", drain($alone, $seen), " $seen rows\n";

echo $db->query('SELECT 42')->fetchColumn(), "\n";
?>
--EXPECT--
query: aborted after 2048 rows
bool(false)
bool(true)
query rerun: completed 5000 rows
exec: aborted after 2048 rows
bool(false)
bool(true)
exec rerun: completed 5000 rows
prepare: aborted after 2048 rows
bool(false)
bool(true)
prepare rerun: completed 5000 rows
single chunk: aborted after 10 rows
no interleave: completed 5000 rows
42
