/* This is a generated file, edit duckdb_driver.stub.php instead.
 * Stub hash: 3ea088042d63d827bb4f2284665f337e38ed17bc */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_PdoDuckDb_Ext_duckdbAppender, 0, 1, Pdo\\Duckdb\\Appender, 0)
	ZEND_ARG_TYPE_INFO(0, table, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, columns, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_METHOD(PdoDuckDb_Ext, duckdbAppender);

static const zend_function_entry class_PdoDuckDb_Ext_methods[] = {
	ZEND_ME(PdoDuckDb_Ext, duckdbAppender, arginfo_class_PdoDuckDb_Ext_duckdbAppender, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};
