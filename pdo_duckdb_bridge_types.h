#ifndef PDO_DUCKDB_BRIDGE_TYPES_H
#define PDO_DUCKDB_BRIDGE_TYPES_H

#include "pdo_duckdb_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pdd_bridge_value pdd_bridge_value;
typedef struct {
    idx_t *path;
    idx_t path_length;
    pdd_bridge_bytes collation;
} pdd_bridge_type_annotation;
typedef struct {
    pdd_bridge_type_annotation *entries;
    idx_t count;
} pdd_bridge_annotations;

/* Copies the native C API result name; release with pdd_bridge_bytes_free. */
pdd_bridge_status pdd_bridge_c_result_column_name(duckdb_result *result, idx_t column, pdd_bridge_bytes *out,
                                                 pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_type_declaration(duckdb_logical_type type, pdd_bridge_bytes *out,
                                             pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_type_alias(duckdb_logical_type type, pdd_bridge_bytes *out, pdd_bridge_error *error);
/* Owns its entries, ordinal paths, and collation bytes; release with annotations_free. */
pdd_bridge_status pdd_bridge_type_annotations(duckdb_logical_type type, pdd_bridge_annotations *out,
                                              pdd_bridge_error *error);
void pdd_bridge_annotations_free(pdd_bridge_annotations *annotations);
pdd_bridge_status pdd_bridge_type_variant(duckdb_logical_type *out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_type_is_tuple(duckdb_logical_type type, bool *out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_type_field_name(duckdb_logical_type type, idx_t field, pdd_bridge_bytes *out,
                                            pdd_bridge_error *error);

/* row_count is the containing chunk's size, not vector capacity. */
pdd_bridge_status pdd_bridge_value_from_vector(duckdb_vector vector, idx_t row, idx_t row_count,
                                               pdd_bridge_value **out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_from_c(duckdb_value value, pdd_bridge_value **out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_to_c(const pdd_bridge_value *value, duckdb_value *out, pdd_bridge_error *error);
void pdd_bridge_value_destroy(pdd_bridge_value **value);
pdd_bridge_status pdd_bridge_value_type(const pdd_bridge_value *value, duckdb_logical_type *out,
                                        pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_is_null(const pdd_bridge_value *value, bool *out, pdd_bridge_error *error);
/* A NULL connection uses DuckDB's default canonical renderer, matching duckdb_get_varchar. */
pdd_bridge_status pdd_bridge_value_render(const pdd_bridge_value *value, duckdb_connection connection,
                                          pdd_bridge_bytes *out, bool *is_null, pdd_bridge_error *error);
/* Preserve hexadecimal GEOMETRY WKB and encoded VARIANT container shape/order at every depth. */
pdd_bridge_status pdd_bridge_value_render_pdo(const pdd_bridge_value *value, duckdb_connection connection,
                                              pdd_bridge_bytes *out, bool *is_null, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_string(const pdd_bridge_value *value, pdd_bridge_bytes *out,
                                          pdd_bridge_error *error);
/* MAP children are entry STRUCTs (key, value); UNION children are its numeric tag and active value. */
pdd_bridge_status pdd_bridge_value_child_count(const pdd_bridge_value *value, idx_t *out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_child(const pdd_bridge_value *value, idx_t index, pdd_bridge_value **out,
                                         pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_cast(const pdd_bridge_value *value, duckdb_logical_type type,
                                       duckdb_connection connection, pdd_bridge_value **out, pdd_bridge_error *error);
/* Strings retain their declared scalar type when cast to VARIANT; they are never parsed as JSON. */
pdd_bridge_status pdd_bridge_value_struct(const pdd_bridge_bytes *names, pdd_bridge_value *const *children,
                                          idx_t count, bool tuple, pdd_bridge_value **out, pdd_bridge_error *error);
pdd_bridge_status pdd_bridge_value_list(duckdb_logical_type child_type, pdd_bridge_value *const *children,
                                        idx_t count, pdd_bridge_value **out, pdd_bridge_error *error);

#ifdef __cplusplus
}
#endif
#endif
