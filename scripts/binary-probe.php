<?php

function checkSame($actual, $expected, string $capability): void
{
    if ($actual !== $expected) {
        throw new RuntimeException($capability . ': expected ' . var_export($expected, true)
            . ', got ' . var_export($actual, true));
    }
}

function checkNative($actual, string $type, $value, string $capability): void
{
    checkSame(get_debug_type($actual), 'stdClass', $capability . ' wrapper');
    checkSame(get_object_vars($actual), ['type' => $type, 'value' => $value], $capability);
}

$extension = new ReflectionExtension('pdo_duckdb');
ob_start();
$extension->info();
$info = ob_get_clean();
$v2 = preg_match('/DuckDB 2\.0 headers\s*=>\s*yes/', $info) === 1;
$expectedV2 = getenv('PDO_DUCKDB_EXPECT_V2');
if ($expectedV2 !== false && $expectedV2 !== '') {
    if ($expectedV2 !== '0' && $expectedV2 !== '1') {
        throw new RuntimeException('PDO_DUCKDB_EXPECT_V2 must be 0 or 1');
    }
    checkSame($v2, $expectedV2 === '1', 'DuckDB 2.0 headers');
}

$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_CONFIG => [
        'autoinstall_known_extensions' => 'false',
        'autoload_known_extensions' => 'false',
    ],
]);
$version = ltrim($db->getAttribute(PDO::ATTR_CLIENT_VERSION), 'v');
$expectedVersion = getenv('PDO_DUCKDB_EXPECT_VERSION');
if ($expectedVersion !== false && $expectedVersion !== '') {
    checkSame($version, ltrim($expectedVersion, 'v'), 'engine version');
}
$bundled = ['autocomplete', 'core_functions', 'icu', 'json', 'parquet'];
if (getenv('PDO_DUCKDB_EXPECT_STATIC') === '1') {
    $bundled[] = 'tpcds';
    $bundled[] = 'tpch';
}
foreach ($bundled as $name) {
    $db->exec('LOAD ' . $name);
}
checkSame(ltrim($db->query('SELECT version()')->fetchColumn(), 'v'), $version, 'core functions');
checkSame($db->query("SELECT json_extract('{\"answer\":42}', '$.answer')")->fetchColumn(), '42', 'JSON');
checkSame($db->query("SELECT strftime(TIMESTAMPTZ '2024-01-02 00:00:00+00' AT TIME ZONE 'America/New_York', '%Y-%m-%d %H:%M:%S')")->fetchColumn(), '2024-01-01 19:00:00', 'timezone');

$directory = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'pdo-duckdb-binary-' . bin2hex(random_bytes(8));
if (!mkdir($directory, 0700)) {
    throw new RuntimeException('Cannot create the Parquet probe directory');
}
$parquet = $directory . DIRECTORY_SEPARATOR . 'values.parquet';
$database = $directory . DIRECTORY_SEPARATOR . 'shared.duckdb';
try {
    $db->exec('CREATE TABLE probe(i INTEGER, s VARCHAR)');
    $insert = $db->prepare('INSERT INTO probe VALUES (:i, :s)');
    $insert->execute(['i' => 42, 's' => 'first']);
    $insert->execute(['i' => 42, 's' => 'second']);
    checkSame($db->exec('COPY probe TO ' . $db->quote($parquet) . ' (FORMAT PARQUET)'), 2, 'Parquet write');
    checkSame($db->query('SELECT sum(i) FROM read_parquet(' . $db->quote($parquet) . ')')->fetchColumn(), '84', 'Parquet read');
    $first = new PDO('duckdb:' . $database);
    $first->exec('CREATE TABLE committed(id INTEGER); INSERT INTO committed VALUES (1)');
    $second = new PDO('duckdb:' . $directory . DIRECTORY_SEPARATOR . '.' . DIRECTORY_SEPARATOR . 'shared.duckdb');
    $second->exec('INSERT INTO committed VALUES (2)');
    checkSame($first->query('SELECT count(*) FROM committed')->fetchColumn(), 2, 'shared committed writes');
    $first->beginTransaction();
    $first->exec('INSERT INTO committed VALUES (99)');
    checkSame($second->query('SELECT count(*) FROM committed')->fetchColumn(), 2, 'connection-local transaction');
    checkSame($first->rollBack(), true, 'explicit rollback');
    checkSame($first->query('SELECT count(*) FROM committed')->fetchColumn(), 2, 'rollback before connection close');
    $second->exec('INSERT INTO committed VALUES (3)');
    unset($first, $second);
    $reopened = new PDO('duckdb:' . $database);
    checkSame($reopened->query('SELECT id FROM committed ORDER BY id')->fetchAll(PDO::FETCH_COLUMN),
        [1, 2, 3], 'shared-instance durable reopen');
    unset($reopened);
} finally {
    unset($first, $second, $reopened);
    foreach ([$parquet, $database, $database . '.wal'] as $file) {
        if (is_file($file) && !unlink($file)) {
            throw new RuntimeException('Cannot remove the binary probe file: ' . $file);
        }
    }
    if (!rmdir($directory)) {
        throw new RuntimeException('Cannot remove the Parquet probe directory');
    }
}

$decimal = $db->query('SELECT 1.000000001::DECIMAL(38,9) AS amount');
checkSame($decimal->getColumnMeta(0)['duckdb:decl_type'], 'DECIMAL(38,9)', 'complete decimal metadata');
checkSame($decimal->fetchColumn(), '1.000000001', 'exact decimal value');
checkSame(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, 1002, 'native structures attribute identifier');
checkSame($db->getAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES), false, 'canonical strings by default');
checkSame($db->query("SELECT ['a'||chr(0)||'b']")->fetchColumn(), "[a\0b]", 'length-aware nested rendering');
checkSame($db->query("SELECT '{\"\":3,\"a\":[1,2]}'::JSON::VARIANT")->fetchColumn(),
    "{'': 3, 'a': [1, 2]}", 'canonical VARIANT object keys');

$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, true);
checkNative($db->query("SELECT struct_pack(a := [1,NULL,3]::INTEGER[3], l := []::INTEGER[],
    d := 1.000000001::DECIMAL(38,9), u := 18446744073709551615::UBIGINT,
    b := from_hex('0061ff'), s := 'a'||chr(0)||'b')")->fetchColumn(),
    'STRUCT(a INTEGER[3], l INTEGER[], d DECIMAL(38,9), u UBIGINT, b BLOB, s VARCHAR)',
    ['a' => [1, null, 3], 'l' => [], 'd' => '1.000000001',
        'u' => '18446744073709551615', 'b' => "\0a\xff", 's' => "a\0b"],
    'lossless native containers');
checkNative($db->query("SELECT (7, 'x', [2,3])")->fetchColumn(),
    ($v2 ? 'TUPLE' : 'STRUCT') . '(INTEGER, VARCHAR, INTEGER[])', [7, 'x', [2, 3]], 'positional tuple');
checkNative($db->query("SELECT MAP(['a'||chr(0)||'b'], [1])")->fetchColumn(), 'MAP(VARCHAR, INTEGER)',
    [['key' => "a\0b", 'value' => 1]], 'native map keys');
checkNative($db->query("SELECT union_value(s := NULL::VARCHAR)")->fetchColumn(), 'UNION(s VARCHAR)',
    ['tag' => 's', 'value' => null], 'native union tag');
checkNative($db->query("SELECT ['[]'::JSON, '{}'::JSON, 'null'::JSON, NULL::JSON]")->fetchColumn(), 'JSON[]',
    [['type' => 'JSON', 'value' => []], ['type' => 'JSON', 'value' => ['type' => 'object', 'value' => []]],
        ['type' => 'JSON', 'value' => null], null], 'JSON shape and null distinctions');

$stream = $db->prepare('SELECT [1,NULL,3]', [PDO::DUCKDB_ATTR_UNBUFFERED => true]);
$stream->execute();
checkNative($stream->fetchColumn(), 'INTEGER[]', [1, null, 3], 'unbuffered native fetch');
$stream->closeCursor();
$canonical = $db->prepare('SELECT [1,2]', [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => false]);
$canonical->execute();
checkSame($canonical->fetchColumn(), '[1, 2]', 'statement canonical override');
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, true);
checkSame($db->query('SELECT [1,2]')->fetchColumn(), '[1, 2]', 'stringify precedence');
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, false);

checkNative($db->query('SELECT 1.000000001::DECIMAL(38,9)::VARIANT')->fetchColumn(),
    'DECIMAL(38,9)', '1.000000001', 'top-level exact VARIANT');
checkNative($db->query("SELECT struct_pack(v := ('a'||chr(0)||'b')::VARIANT)")->fetchColumn(),
    'STRUCT(v VARIANT)', ['v' => ['type' => 'VARCHAR', 'value' => "a\0b"]], 'nested VARIANT with NUL');
checkNative($db->query('SELECT [NULL::VARIANT]::VARIANT')->fetchColumn(),
    'ARRAY', [['type' => 'NULL', 'value' => null]], 'VARIANT null');
checkSame($db->query('SELECT NULL::VARIANT')->fetchColumn(), null, 'SQL VARIANT null');

printf("PROBE-OK: DuckDB %s; v2 headers=%s; core, JSON, Parquet, timezone, bundled extensions, shared writes, metadata, and lossless fetches\n", $version, $v2 ? 'yes' : 'no');
