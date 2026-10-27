#ifndef PDO_DUCKDB_BRIDGE_APPENDER_H
#define PDO_DUCKDB_BRIDGE_APPENDER_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero segments select main; one is literal; multiple segments select a GA nested schema. */
/* On success, use duckdb_appender_destroy() to release the returned v1 C API handle. */
pdd_bridge_status pdd_bridge_appender_create(duckdb_connection connection,
    const pdd_bridge_bytes *schema_segments, size_t schema_count, pdd_bridge_bytes table,
    duckdb_appender *out, pdd_bridge_error *error);

#ifdef __cplusplus
}
#endif

#endif
