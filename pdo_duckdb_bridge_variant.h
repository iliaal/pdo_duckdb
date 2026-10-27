#ifndef PDO_DUCKDB_BRIDGE_VARIANT_H
#define PDO_DUCKDB_BRIDGE_VARIANT_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdd_bridge_variant pdd_bridge_variant;
typedef enum {
    PDD_BRIDGE_VARIANT_SQL_NULL = 0,
    PDD_BRIDGE_VARIANT_NULL = 1,
    PDD_BRIDGE_VARIANT_SCALAR = 2,
    PDD_BRIDGE_VARIANT_OBJECT = 3,
    PDD_BRIDGE_VARIANT_ARRAY = 4
} pdd_bridge_variant_kind;

/* Each node owns a reference to its immutable backing value; children outlive their parent handles. */
pdd_bridge_status pdd_bridge_variant_from_c(duckdb_value value, pdd_bridge_variant **out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_variant_from_vector(duckdb_vector vector, idx_t row, idx_t row_count,
                                                 pdd_bridge_variant **out, pdd_bridge_error *error);
void pdd_bridge_variant_destroy(pdd_bridge_variant **value);
pdd_bridge_status pdd_bridge_variant_info(const pdd_bridge_variant *value, pdd_bridge_variant_kind *kind,
                                          idx_t *child_count, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_variant_child(const pdd_bridge_variant *value, idx_t index, pdd_bridge_variant **out,
                                           pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_variant_key(const pdd_bridge_variant *value, idx_t index, pdd_bridge_bytes *out,
                                         pdd_bridge_error *error);
/* Only scalars and either null kind convert to a C value. Containers retain their distinct node kind. */
pdd_bridge_status pdd_bridge_variant_scalar(const pdd_bridge_variant *value, duckdb_value *out,
                                            pdd_bridge_error *error);
/* Children must be SQL scalars or VARIANT values. VARCHAR is never interpreted as JSON. */
pdd_bridge_status pdd_bridge_variant_object(const pdd_bridge_bytes *keys, const duckdb_value *children, idx_t count,
                                            duckdb_value *out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_variant_array(const duckdb_value *children, idx_t count, duckdb_value *out,
                                           pdd_bridge_error *error);

#ifdef __cplusplus
}
#endif
#endif
