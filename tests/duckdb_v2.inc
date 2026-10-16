<?php
/* True when pdo_duckdb was compiled against the DuckDB 2.0 headers
 * (HAVE_DUCKDB_V2), read from the compile-time MINFO row. */
function pdo_duckdb_v2_headers(): bool
{
    ob_start();
    (new ReflectionExtension('pdo_duckdb'))->info();
    return str_contains(ob_get_clean(), 'DuckDB 2.0 headers => yes');
}

/* DuckDB 2.0 groups profiling metrics into unnamed category children of the
 * root; return the first child whose metrics carry $key. */
function pdo_duckdb_profile_group(array $profile, string $key): ?array
{
    foreach ($profile['children'] as $child) {
        if (array_key_exists($key, $child['metrics'])) {
            return $child;
        }
    }
    return null;
}

function pdo_duckdb_profile_has_operator(array $node, string $type): bool
{
    if (($node['metrics']['type'] ?? null) === $type) {
        return true;
    }
    foreach ($node['children'] as $child) {
        if (pdo_duckdb_profile_has_operator($child, $type)) {
            return true;
        }
    }
    return false;
}
