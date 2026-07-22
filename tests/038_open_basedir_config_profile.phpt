--TEST--
pdo_duckdb: open_basedir rejects path/security DuckDB config and locks runtime config
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$blocked = [
    'allowed_directories',
    'allowed_paths',
    'allowed_configs',
    'file_search_path',
    'temp_directory',
    'extension_directory',
    'extension_directories',
    'custom_extension_repository',
    'autoinstall_extension_repository',
    'autoinstall_known_extensions',
    'autoload_known_extensions',
    'allow_community_extensions',
    'allow_extensions_metadata_mismatch',
    'allow_persistent_secrets',
    'allow_unredacted_secrets',
    'allow_unsigned_extensions',
    'default_secret_storage',
    'enable_external_file_cache',
    'enable_http_metadata_cache',
    'home_directory',
    'http_logging_output',
    'log_query_path',
    'secret_directory',
];

foreach (['array', 'dsn'] as $source) {
    $count = 0;
    foreach ($blocked as $key) {
        try {
            $options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
            $dsn = 'duckdb::memory:';
            $value = $key === 'temp_directory' ? __DIR__ . '/sandbox-temp' : 'sandbox-test';
            if ($source === 'array') {
                $options[PDO::DUCKDB_ATTR_CONFIG] = [$key => $value];
            } else {
                $dsn .= ";$key=$value";
            }
            new PDO($dsn, null, null, $options);
            echo "BAD: $source $key accepted\n";
        } catch (PDOException $e) {
            $expected = "DuckDB configuration option \"$key\" is not allowed when open_basedir is set";
            if (!str_contains($e->getMessage(), $expected)) {
                echo "BAD: $source $key wrong error: ", $e->getMessage(), "\n";
                continue;
            }
            $count++;
        }
    }
    echo "$source blocked=$count\n";
}

$db = new PDO('duckdb::memory:;enable_external_access=true', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_CONFIG => ['threads' => 1],
]);

foreach ([
    'enable_external_access',
    'autoinstall_known_extensions',
    'autoload_known_extensions',
    'allow_community_extensions',
    'allow_persistent_secrets',
    'enable_external_file_cache',
    'lock_configuration',
] as $setting) {
    echo $setting, '=', $db->query("SELECT current_setting('$setting')::VARCHAR")->fetchColumn(), "\n";
}

try {
    $db->exec("SET memory_limit='1GB'");
    echo "BAD: runtime config changed\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'configuration has been locked') ? "runtime config locked\n" : $e->getMessage() . "\n";
}
?>
--EXPECT--
array blocked=23
dsn blocked=23
enable_external_access=false
autoinstall_known_extensions=false
autoload_known_extensions=false
allow_community_extensions=false
allow_persistent_secrets=false
enable_external_file_cache=false
lock_configuration=true
runtime config locked
