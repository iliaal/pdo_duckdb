#ifndef PDO_DUCKDB_STATIC_EXTENSIONS_H
#define PDO_DUCKDB_STATIC_EXTENSIONS_H

#ifdef HAVE_DUCKDB_STATIC_EXTENSIONS
int pdo_duckdb_register_static_extensions(void);
#else
static inline int pdo_duckdb_register_static_extensions(void)
{
	return 0;
}
#endif

#endif
