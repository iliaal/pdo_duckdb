<?php

/** @generate-class-entries */

namespace Pdo\Duckdb {
    /**
     * Fast bulk-insert handle for a single table, created via
     * PDO::duckdbAppender() / Pdo\Duckdb::duckdbAppender(). Wraps DuckDB's
     * native appender API.
     *
     * @strict-properties
     * @not-serializable
     */
    final class Appender
    {
        private function __construct() {}

        /** Append one row; each argument is a column value (left to right). */
        public function appendRow(mixed ...$values): static {}

        /** Flush buffered rows to the table. */
        public function flush(): void {}

        /** Flush and finalize; the appender is unusable afterwards. */
        public function close(): void {}
    }
}

namespace Pdo {
    /**
     * Driver-specific PDO subclass for DuckDB. On PHP 8.4+, PDO::connect()
     * with a duckdb: DSN returns an instance of this class (registered as the
     * driver-specific CE); new PDO() continues to return the base PDO class.
     * Its methods do not trip the 8.5 deprecation of base-PDO driver methods.
     * On 8.1-8.3 the same method is exposed on the base PDO object via
     * get_driver_methods instead.
     */
    class Duckdb extends \PDO
    {
        /**
         * Create a bulk-insert appender for the given table. When $columns is
         * given, the appender targets only those columns (in that order) and any
         * omitted column is filled with its DEFAULT, or NULL.
         */
        public function duckdbAppender(string $table, ?string $schema = null, ?array $columns = null): \Pdo\Duckdb\Appender {}

        /**
         * Return the tables a query references, using DuckDB's parser. Read
         * queries only; DML (INSERT/UPDATE/DELETE) yields an empty array. With
         * $qualified, a non-default schema is included (e.g. "s.orders").
         * Throws if the query cannot be parsed.
         */
        public function duckdbTableNames(string $query, bool $qualified = false): array {}

        /**
         * Return the profiling tree of the last executed query as a nested
         * array shaped ['metrics' => array<string,string>, 'children' => list],
         * or null if profiling is not enabled. Enable it first with
         * PRAGMA enable_profiling (the method does not execute any query).
         */
        public function duckdbLastProfile(): ?array {}
    }
}
