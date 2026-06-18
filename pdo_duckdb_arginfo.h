/* This is a generated file, edit pdo_duckdb.stub.php instead.
 * Stub hash: 81fd2ff5eb2e83699a2bfd216e33d2ca86465642 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_Pdo_Duckdb_Appender___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_Pdo_Duckdb_Appender_appendRow, 0, 0, IS_STATIC, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, values, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_Pdo_Duckdb_Appender_flush, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_Pdo_Duckdb_Appender_close arginfo_class_Pdo_Duckdb_Appender_flush

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_Pdo_Duckdb_duckdbAppender, 0, 1, Pdo\\Duckdb\\Appender, 0)
	ZEND_ARG_TYPE_INFO(0, table, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_METHOD(Pdo_Duckdb_Appender, __construct);
ZEND_METHOD(Pdo_Duckdb_Appender, appendRow);
ZEND_METHOD(Pdo_Duckdb_Appender, flush);
ZEND_METHOD(Pdo_Duckdb_Appender, close);
ZEND_METHOD(Pdo_Duckdb, duckdbAppender);

static const zend_function_entry class_Pdo_Duckdb_Appender_methods[] = {
	ZEND_ME(Pdo_Duckdb_Appender, __construct, arginfo_class_Pdo_Duckdb_Appender___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(Pdo_Duckdb_Appender, appendRow, arginfo_class_Pdo_Duckdb_Appender_appendRow, ZEND_ACC_PUBLIC)
	ZEND_ME(Pdo_Duckdb_Appender, flush, arginfo_class_Pdo_Duckdb_Appender_flush, ZEND_ACC_PUBLIC)
	ZEND_ME(Pdo_Duckdb_Appender, close, arginfo_class_Pdo_Duckdb_Appender_close, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_Pdo_Duckdb_methods[] = {
	ZEND_ME(Pdo_Duckdb, duckdbAppender, arginfo_class_Pdo_Duckdb_duckdbAppender, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_Pdo_Duckdb_Appender(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "Pdo\\Duckdb", "Appender", class_Pdo_Duckdb_Appender_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	return class_entry;
}

static zend_class_entry *register_class_Pdo_Duckdb(zend_class_entry *class_entry_PDO)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "Pdo", "Duckdb", class_Pdo_Duckdb_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_PDO, 0);

	return class_entry;
}
