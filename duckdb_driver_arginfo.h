/* This is a generated file, edit duckdb_driver.stub.php instead.
 * Stub hash: 7f99c6c6d40793254202a72879ec6d29ab7b2ea4 */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_PdoDuckDb_Ext_duckdbAppender, 0, 1, Pdo\\Duckdb\\Appender, 0)
	ZEND_ARG_TYPE_INFO(0, table, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_METHOD(PdoDuckDb_Ext, duckdbAppender);

static const zend_function_entry class_PdoDuckDb_Ext_methods[] = {
	ZEND_ME(PdoDuckDb_Ext, duckdbAppender, arginfo_class_PdoDuckDb_Ext_duckdbAppender, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};
