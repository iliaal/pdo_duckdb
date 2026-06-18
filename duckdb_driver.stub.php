<?php

/** @generate-function-entries */

/**
 * Driver-specific methods attached to a PDO instance when the driver is
 * "duckdb". This is not a real class.
 * @undocumentable
 */
class PdoDuckDb_Ext
{
    /** Create a bulk-insert appender for the given table. */
    public function duckdbAppender(string $table, ?string $schema = null): \Pdo\Duckdb\Appender {}
}
