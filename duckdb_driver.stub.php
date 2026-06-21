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
}
