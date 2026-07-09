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
    'allowed_directories' => '/etc',
    'allowed_paths' => '/etc/hostname',
    'temp_directory' => '/tmp/pdo-duckdb-temp-outside',
    'extension_directory' => '/tmp/pdo-duckdb-ext-outside',
    'home_directory' => '/tmp/pdo-duckdb-home-outside',
    'autoinstall_known_extensions' => 'true',
];

foreach ($blocked as $key => $value) {
    try {
        new PDO('duckdb::memory:', null, null, [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::DUCKDB_ATTR_CONFIG => [$key => $value],
        ]);
        echo "BAD: $key accepted\n";
    } catch (PDOException $e) {
        echo "$key blocked\n";
    }
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
allowed_directories blocked
allowed_paths blocked
temp_directory blocked
extension_directory blocked
home_directory blocked
autoinstall_known_extensions blocked
enable_external_access=false
autoinstall_known_extensions=false
autoload_known_extensions=false
allow_community_extensions=false
allow_persistent_secrets=false
enable_external_file_cache=false
lock_configuration=true
runtime config locked
