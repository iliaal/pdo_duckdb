<?php

/** @generate-function-entries */

/**
 * Driver-specific methods attached to a PDO instance when the driver is
 * "duckdb". This is not a real class.
 * @undocumentable
 */
class PdoDuckDb_Ext
{
    /**
     * Create a bulk-insert appender for the given table. When $columns is given,
     * the appender targets only those columns (in that order); omitted columns
     * are filled with their DEFAULT, or NULL.
     */
    public function duckdbAppender(string $table, ?string $schema = null, ?array $columns = null): \Pdo\Duckdb\Appender {}

    /**
     * Return the tables a query references, using DuckDB's parser. Read queries
     * only; DML yields an empty array. With $qualified, a non-default schema is
     * included (e.g. "s.orders"). Throws if the query cannot be parsed.
     */
    public function duckdbTableNames(string $query, bool $qualified = false): array {}

    /**
     * Return the profiling tree of the last executed query as a nested array
     * shaped ['metrics' => array<string,string>, 'children' => list], or null
     * if profiling is not enabled. Enable it first with PRAGMA enable_profiling.
     */
    public function duckdbLastProfile(): ?array {}
}
