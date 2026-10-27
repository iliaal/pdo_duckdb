--TEST--
pdo_duckdb: interleaved statements preserve the engine's unbuffered result contract
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

function drain(PDOStatement $stmt, int &$seen): string
{
    try {
        while (($v = $stmt->fetchColumn()) !== false) {
            if ((int) $v !== $seen) {
                throw new RuntimeException("row $seen read back as $v");
            }
            $seen++;
        }
        return 'completed';
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'Attempting to execute an unsuccessful or closed')
            || $e->errorInfo[1] !== 5) {
            throw $e;
        }
        return 'aborted';
    }
}

function interleave(callable $other, bool $v2): void
{
    try {
        $other();
        if ($v2) {
            throw new RuntimeException('another statement accepted with an open result');
        }
    } catch (PDOException $e) {
        if (!$v2 || !str_contains($e->getMessage(), 'connection has an open result')) {
            throw $e;
        }
    }
}

$interleave = [
    'query' => fn() => $db->query('SELECT 99')->fetchColumn(),
    'exec' => fn() => $db->exec('CREATE TEMP TABLE IF NOT EXISTS scratch (i INTEGER)'),
    'prepare' => fn() => $db->prepare('SELECT 1'),
];

foreach ($interleave as $label => $other) {
    $first = $db->query('SELECT i FROM range(5000) t(i)');
    if ($first->fetchColumn() !== 0) {
        throw new RuntimeException('first row was not zero');
    }
    interleave($other, $v2);
    $seen = 1;
    $status = drain($first, $seen);
    if ($status !== ($v2 ? 'completed' : 'aborted') || $seen !== ($v2 ? 5000 : 2048)) {
        throw new RuntimeException("$label: $status after $seen rows");
    }
    if ($first->fetchColumn() !== false || !$first->closeCursor()) {
        throw new RuntimeException('finished result did not close');
    }
    echo "$label: interleave contract verified\n";
    $first->execute();
    $seen = 0;
    echo "$label rerun: ", drain($first, $seen), " $seen rows\n";
}

$small = $db->query('SELECT i FROM range(10) t(i)');
$small->fetchColumn();
interleave($interleave['query'], $v2);
$seen = 1;
$status = drain($small, $seen);
if ($status !== ($v2 ? 'completed' : 'aborted') || $seen !== 10) {
    throw new RuntimeException("single chunk: $status after $seen rows");
}
echo "single chunk: interleave contract verified\n";

$alone = $db->query('SELECT i FROM range(5000) t(i)');
$seen = 0;
echo 'no interleave: ', drain($alone, $seen), " $seen rows\n";

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$quiet = $db->query('SELECT i FROM range(5000) t(i)');
$quiet->fetchColumn();
$other = $db->query('SELECT 99');
if ($v2 && ($other !== false || $db->errorCode() !== '42000' || $db->errorInfo()[1] !== 3)) {
    throw new RuntimeException('silent interleave did not report the preparation error');
}
$seen = 1;
while (($v = $quiet->fetchColumn()) !== false) {
    if ($v !== $seen) {
        throw new RuntimeException("silent row $seen read back as $v");
    }
    $seen++;
}
if ($v2) {
    if ($seen !== 5000 || $quiet->errorCode() !== '00000') {
        throw new RuntimeException('rejected statement damaged the original stream');
    }
} elseif ($seen !== 2048 || $quiet->errorCode() !== 'HY000' || $quiet->errorInfo()[1] !== 5) {
    throw new RuntimeException('legacy stream did not report its interleave failure');
}
echo "silent: interleave contract verified\n";
echo $db->query('SELECT 42')->fetchColumn(), "\n";
?>
--EXPECT--
query: interleave contract verified
query rerun: completed 5000 rows
exec: interleave contract verified
exec rerun: completed 5000 rows
prepare: interleave contract verified
prepare rerun: completed 5000 rows
single chunk: interleave contract verified
no interleave: completed 5000 rows
silent: interleave contract verified
42
