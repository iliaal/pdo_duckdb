#ifndef PDO_DUCKDB_BRIDGE_H
#define PDO_DUCKDB_BRIDGE_H

#include <duckdb.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PDD_BRIDGE_OK = 0, PDD_BRIDGE_ERROR = 1 } pdd_bridge_status;
typedef struct { char *data; size_t length; } pdd_bridge_bytes;
typedef struct { char *data; size_t length; } pdd_bridge_error;

/* Outputs start empty. Free owned outputs before reusing an output slot. */
pdd_bridge_status pdd_bridge_check(pdd_bridge_error *error);
void pdd_bridge_bytes_free(pdd_bridge_bytes *bytes);
void pdd_bridge_error_free(pdd_bridge_error *error);

#ifdef __cplusplus
}
#endif
#endif
