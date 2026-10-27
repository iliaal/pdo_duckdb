#ifndef PDO_DUCKDB_BRIDGE_LOGGING_H
#define PDO_DUCKDB_BRIDGE_LOGGING_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stop instance-wide local file logging without SQL dispatch or changing a transaction/result.
 * Retained file-storage binders and background loggers cannot write to the old files after
 * this function returns. An in-flight file operation may finish before revocation acquires
 * the storage lock; later retained binders fail without opening or writing a file.
 * Existing in-memory logging remains enabled and retains its records.
 */
pdd_bridge_status pdd_bridge_clear_instance_writers(duckdb_connection connection, pdd_bridge_error *error);

#ifdef __cplusplus
}
#endif

#endif
