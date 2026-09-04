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
     *
     * Throws ValueError for a NUL byte in $table/$schema/column names and for
     * an empty $columns list, TypeError for non-string column names.
     * Error("Pdo\Duckdb\Appender is closed"); close() is not idempotent. A
     * failed native append/flush/close poisons the appender: flushed rows
     * survive, buffered-but-unflushed rows are lost. Probe rejections
     * ("Failed to append value: ...") and soft validation failures do not
     * poison: the appender stays usable.
     */
    public function duckdbAppender(string $table, ?string $schema = null, ?array $columns = null): \Pdo\Duckdb\Appender {}

    /**
     * Return the tables a query references, using DuckDB's parser. Read queries
     * only; DML yields an empty array, DDL behavior is unspecified. With
     * $qualified, a non-default schema is included (e.g. "s.orders"). A NUL
     * byte in $query throws ValueError; a query that cannot be parsed throws
     * PDOException with a detail-free message (prepare the query for specifics).
     */
    public function duckdbTableNames(string $query, bool $qualified = false): array {}

    /**
     * Return the profiling tree of the last executed query as a nested array
     * shaped ['metrics' => array<string, string|null>, 'children' => list], or
     * null if profiling is not enabled. Enable it first with PRAGMA
     * enable_profiling. A metric whose DuckDB value is SQL NULL becomes PHP null.
     */
    public function duckdbLastProfile(): ?array {}
}
