#ifndef PDO_DUCKDB_BRIDGE_FILESYSTEM_H
#define PDO_DUCKDB_BRIDGE_FILESYSTEM_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install before opening through the process's shared instance cache.
 * The engine owns the filesystem after consuming the config. */
pdd_bridge_status pdd_bridge_filesystem_install(duckdb_config config, pdd_bridge_error *error);

/* Capture newly created primary files immediately after a successful cached open. */
pdd_bridge_status pdd_bridge_filesystem_refresh(duckdb_database database, pdd_bridge_error *error);

/* A changed representative remains refused until every filesystem owner that used it closes,
 * including after DETACH. Failed queries release only their still-missing pending references.
 * Uncertain identity samples leave subsequent local database opens fail-closed until sampling
 * succeeds. Concurrent external replacement/ABA is unsupported;
 * identity is sampled from paths, rather than obtained from the engine's open native file handles. */

#ifdef __cplusplus
}
#endif
#endif
