--TEST--
pdo_duckdb: embedded NUL bytes in PDO::DUCKDB_ATTR_CONFIG keys/values are rejected, not truncated
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect(array $opts) {
    return PHP_VERSION_ID >= 80400
        ? PDO::connect('duckdb::memory:', null, null, $opts)
        : new PDO('duckdb::memory:', null, null, $opts);
}

// A NUL in the option name would truncate "threads\0x" to "threads" and apply an
// unintended option — reject it.
try {
    connect([PDO::DUCKDB_ATTR_CONFIG => ["threads\0not_a_real_option" => "2"]]);
    echo "BAD: NUL key truncated+applied\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'NUL byte'));
}

// A NUL in the value would truncate "2\0bad" to "2".
try {
    connect([PDO::DUCKDB_ATTR_CONFIG => ["threads" => "2\0not_a_number"]]);
    echo "BAD: NUL value truncated+applied\n";
} catch (\PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'NUL byte'));
}

// A clean config still applies.
$db = connect([PDO::DUCKDB_ATTR_CONFIG => ["threads" => "2"]]);
var_dump((int) $db->query("SELECT current_setting('threads')")->fetchColumn());
?>
--EXPECT--
bool(true)
bool(true)
int(2)
