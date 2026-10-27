#ifdef HAVE_CONFIG_H
#include <config.h>
#elif defined(PHP_WIN32)
#include <php.h>
#endif

#include "duckdb_static_extensions.h"

#ifdef HAVE_DUCKDB_STATIC_EXTENSIONS
#include <duckdb_static_extension.h>

#define PDO_DUCKDB_STATIC_EXTENSION_LIST(X) \
	X(autocomplete) \
	X(core_functions) \
	X(icu) \
	X(json) \
	X(parquet) \
	X(tpcds) \
	X(tpch) \
	X(httplib) \
	X(loadable_extensions)

#define PDO_DUCKDB_DECLARE_EXTENSION(name) \
	extern int32_t duckdb_extension_##name##_describe(duckdb_extension_descriptor *descriptor);
PDO_DUCKDB_STATIC_EXTENSION_LIST(PDO_DUCKDB_DECLARE_EXTENSION)

int pdo_duckdb_register_static_extensions(void)
{
#define PDO_DUCKDB_EXTENSION_ENTRY(name) duckdb_extension_##name##_describe,
	static const duckdb_extension_describe_t extensions[] = {
		PDO_DUCKDB_STATIC_EXTENSION_LIST(PDO_DUCKDB_EXTENSION_ENTRY)
	};
	unsigned int i;

	for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		if (duckdb_register_static_extension(extensions[i]) != 0) {
			return -1;
		}
	}
	return 0;
}
#endif
