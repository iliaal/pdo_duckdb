<?php
declare(strict_types=1);

function open_database(string $path, string $compatibility = ''): PDO
{
    $options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
    if ($compatibility === 'array') {
        $options[PDO::DUCKDB_ATTR_CONFIG] = ['storage_compatibility_version' => 'v1.5.6'];
    }
    $dsn = 'duckdb:' . $path;
    if ($compatibility === 'dsn') {
        $dsn .= ';storage_compatibility_version=v1.5.3';
    }
    return new PDO($dsn, null, null, $options);
}

function rows(PDO $db, string $table = 'records'): array
{
    return $db->query("SELECT id, marker, hex(payload) FROM $table ORDER BY id")->fetchAll(PDO::FETCH_NUM);
}

function worker(string $operation, string $path, string $marker, string $compatibility): mixed
{
    if ($operation === 'version') {
        return open_database(':memory:')->query('SELECT version()')->fetchColumn();
    }
    if ($operation === 'reject') {
        try {
            open_database($path);
        } catch (PDOException $e) {
            return $e->getMessage();
        }
        throw new RuntimeException('old engine opened a new-format database');
    }
    $table = 'records';
    if ($compatibility === 'attach') {
        $db = open_database(':memory:');
        $db->exec('ATTACH ' . $db->quote($path) . " AS shared (STORAGE_VERSION 'v1.5.3')");
        $table = 'shared.records';
    } else {
        $db = open_database($path, $compatibility);
    }
    if ($operation === 'create') {
        $db->exec("CREATE TABLE $table (id INTEGER PRIMARY KEY, marker VARCHAR, payload BLOB)");
        $stmt = $db->prepare("INSERT INTO $table VALUES (1, ?, ?)");
        $stmt->bindValue(1, $marker);
        $stmt->bindValue(2, "old\0\xff", PDO::PARAM_LOB);
        $stmt->execute();
        $db->exec($compatibility === 'attach' ? 'CHECKPOINT shared' : 'CHECKPOINT');
    } elseif ($operation === 'write') {
        $db->beginTransaction();
        $stmt = $db->prepare("UPDATE $table SET marker = ? WHERE id = 1");
        $stmt->execute([$marker]);
        $stmt = $db->prepare("INSERT INTO $table VALUES (2, ?, ?)");
        $stmt->bindValue(1, $marker . '/new');
        $stmt->bindValue(2, "new\0\xfe", PDO::PARAM_LOB);
        $stmt->execute();
        $db->commit();
        $db->exec('CHECKPOINT');
    } elseif ($operation !== 'read') {
        throw new InvalidArgumentException('unknown worker operation');
    }
    return rows($db, $table);
}

if (($argv[1] ?? '') === '--worker') {
    try {
        $result = worker($argv[2], $argv[3], $argv[4], $argv[5]);
        echo json_encode($result, JSON_THROW_ON_ERROR), "\n";
        exit(0);
    } catch (Throwable $e) {
        fwrite(STDERR, $e::class . ': ' . $e->getMessage() . "\n");
        exit(1);
    }
}

if ($argc !== 2 || $argv[1] === '--help') {
    fwrite(STDERR, "Usage: php scripts/storage-interop.php runners.json\n"
        . "runners.json maps 1.5.3, 1.5.6, and 2.0.0 to {\"command\":[\"/path/to/php\",\"-n\",\"-d\",\"extension=pdo\",\"-d\",\"extension=/path/to/pdo_duckdb.so\"],\"env\":{\"LD_LIBRARY_PATH\":\"/path/to/duckdb/lib\"}}.\n");
    exit($argc === 2 ? 0 : 1);
}

function run_engine(array $runners, string $version, string $operation, string $path = ':memory:',
    string $marker = '', string $compatibility = ''): mixed
{
    $runner = $runners[$version];
    $command = array_merge($runner['command'], [__FILE__, '--worker', $operation, $path, $marker, $compatibility]);
    $process = proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
        $pipes, null, array_merge(getenv(), $runner['env'] ?? []));
    if (!is_resource($process)) {
        throw new RuntimeException("$version: could not start PHP");
    }
    fclose($pipes[0]);
    $output = stream_get_contents($pipes[1]);
    $error = stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    $status = proc_close($process);
    if ($status !== 0 || $error !== '') {
        throw new RuntimeException("$version $operation failed ($status): $output$error");
    }
    return json_decode($output, true, 512, JSON_THROW_ON_ERROR);
}

function expect_rows(mixed $actual, array $expected, string $context): void
{
    if ($actual !== $expected) {
        throw new RuntimeException($context . ': rows differ: ' . json_encode($actual, JSON_THROW_ON_ERROR));
    }
}

$directory = null;
$status = 0;
try {
    $runners = json_decode(file_get_contents($argv[1]), true, 512, JSON_THROW_ON_ERROR);
    foreach (['1.5.3', '1.5.6', '2.0.0'] as $version) {
        $runner = $runners[$version] ?? [];
        if (!isset($runner['command']) || !is_array($runner['command'])
            || !array_is_list($runner['command']) || $runner['command'] === []) {
            throw new InvalidArgumentException("$version: command must be a nonempty argv array");
        }
        foreach ($runner['command'] as $argument) {
            if (!is_string($argument)) {
                throw new InvalidArgumentException("$version: command arguments must be strings");
            }
        }
        foreach (($runner['env'] ?? []) as $name => $value) {
            if (!is_string($name) || !is_string($value)) {
                throw new InvalidArgumentException("$version: env must map names to strings");
            }
        }
        $actual = run_engine($runners, $version, 'version');
        if ($actual !== 'v' . $version) {
            throw new RuntimeException("$version: loaded $actual");
        }
        echo "engine $version verified\n";
    }
    $directory = sys_get_temp_dir() . '/pdo-duckdb-storage-' . bin2hex(random_bytes(8));
    if (!mkdir($directory, 0700)) {
        throw new RuntimeException('could not create the temporary database directory');
    }
    $nonce = bin2hex(random_bytes(16));
    foreach (['1.5.3', '1.5.6'] as $creator) {
        $path = "$directory/old-$creator.db";
        $marker = "$nonce/$creator";
        $initial = [[1, $marker, '6F6C6400FF']];
        expect_rows(run_engine($runners, $creator, 'create', $path, $marker), $initial, "$creator create");
        expect_rows(run_engine($runners, '2.0.0', 'read', $path), $initial, "$creator read by GA");
        $marker .= '/GA';
        $updated = [[1, $marker, '6F6C6400FF'], [2, "$marker/new", '6E657700FE']];
        expect_rows(run_engine($runners, '2.0.0', 'write', $path, $marker), $updated, "$creator GA write");
        foreach (['1.5.3', '1.5.6', '2.0.0'] as $reader) {
            expect_rows(run_engine($runners, $reader, 'read', $path), $updated, "$creator reopened by $reader");
        }
        echo "$creator file: GA update and insert survive both old readers\n";
    }
    foreach (['dsn', 'array', 'attach'] as $compatibility) {
        $path = "$directory/compatible-$compatibility.db";
        $marker = "$nonce/$compatibility";
        $expected = [[1, $marker, '6F6C6400FF']];
        expect_rows(run_engine($runners, '2.0.0', 'create', $path, $marker, $compatibility), $expected,
            "$compatibility create");
        foreach (['1.5.3', '1.5.6', '2.0.0'] as $reader) {
            expect_rows(run_engine($runners, $reader, 'read', $path), $expected, "$compatibility read by $reader");
        }
        echo "GA $compatibility compatibility: both old readers preserve data\n";
    }
    $path = "$directory/default.db";
    $marker = "$nonce/default";
    $expected = [[1, $marker, '6F6C6400FF']];
    expect_rows(run_engine($runners, '2.0.0', 'create', $path, $marker), $expected, 'GA default create');
    foreach (['1.5.3', '1.5.6'] as $reader) {
        $error = run_engine($runners, $reader, 'reject', $path);
        if (!str_contains($error, 'Trying to read a database file with version number')
            || !str_contains($error, 'but we can only read versions between')) {
            throw new RuntimeException("$reader: unexpected new-format rejection: $error");
        }
    }
    expect_rows(run_engine($runners, '2.0.0', 'read', $path), $expected, 'GA default after old-reader rejection');
    echo "GA default format: both old readers reject it; GA data remains intact\n";
} catch (Throwable $e) {
    fwrite(STDERR, $e::class . ': ' . $e->getMessage() . "\n");
    $status = 1;
} finally {
    if ($directory !== null && is_dir($directory)) {
        foreach (glob($directory . '/*') as $path) {
            unlink($path);
        }
        rmdir($directory);
    }
}
exit($status);
