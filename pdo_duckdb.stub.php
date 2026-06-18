<?php

/** @generate-class-entries */

namespace Pdo\Duckdb {
    /**
     * Fast bulk-insert handle for a single table, created via
     * PDO::duckdbAppender(). Wraps DuckDB's native appender API.
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
