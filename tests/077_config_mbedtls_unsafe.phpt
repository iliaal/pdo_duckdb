--TEST--
pdo_duckdb: force_mbedtls_unsafe is rejected at connect time
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Falsy values SIGSEGV inside libduckdb 1.5.3-1.5.5 at duckdb_set_config.
// Reject the key unconditionally so neither DSN nor ATTR_CONFIG can reach it.

function expect_connect_time($label, callable $fn): void
{
    try {
        $fn();
        echo "$label accepted (BUG)\n";
    } catch (PDOException $e) {
        echo $label, ': ',
            str_contains($e->getMessage(), 'force_mbedtls_unsafe')
            && str_contains($e->getMessage(), 'connect time')
                ? 'rejected'
                : ('other=' . $e->getMessage()),
            "\n";
    }
}

expect_connect_time('dsn_false', fn() => new PDO('duckdb::memory:;force_mbedtls_unsafe=false'));
expect_connect_time('dsn_0', fn() => new PDO('duckdb::memory:;force_mbedtls_unsafe=0'));
expect_connect_time('dsn_true', fn() => new PDO('duckdb::memory:;force_mbedtls_unsafe=true'));
expect_connect_time('attr_false', fn() => new PDO('duckdb::memory:', null, null, [
    PDO::DUCKDB_ATTR_CONFIG => ['force_mbedtls_unsafe' => false],
]));
expect_connect_time('attr_str', fn() => new PDO('duckdb::memory:', null, null, [
    PDO::DUCKDB_ATTR_CONFIG => ['force_mbedtls_unsafe' => 'false'],
]));

// A normal connection still works.
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
echo 'open=', (int)$db->query('SELECT 1')->fetchColumn(), "\n";
?>
--EXPECT--
dsn_false: rejected
dsn_0: rejected
dsn_true: rejected
attr_false: rejected
attr_str: rejected
open=1
