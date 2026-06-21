/* This is a generated file, edit duckdb_driver.stub.php instead.
 * Stub hash: 85503783e36958bdfcde552cb981d3b521fe9707 */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_PdoDuckDb_Ext_duckdbAppender, 0, 1, Pdo\\Duckdb\\Appender, 0)
	ZEND_ARG_TYPE_INFO(0, table, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, columns, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PdoDuckDb_Ext_duckdbTableNames, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, query, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, qualified, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PdoDuckDb_Ext_duckdbLastProfile, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_METHOD(PdoDuckDb_Ext, duckdbAppender);
ZEND_METHOD(PdoDuckDb_Ext, duckdbTableNames);
ZEND_METHOD(PdoDuckDb_Ext, duckdbLastProfile);

static const zend_function_entry class_PdoDuckDb_Ext_methods[] = {
	ZEND_ME(PdoDuckDb_Ext, duckdbAppender, arginfo_class_PdoDuckDb_Ext_duckdbAppender, ZEND_ACC_PUBLIC)
	ZEND_ME(PdoDuckDb_Ext, duckdbTableNames, arginfo_class_PdoDuckDb_Ext_duckdbTableNames, ZEND_ACC_PUBLIC)
	ZEND_ME(PdoDuckDb_Ext, duckdbLastProfile, arginfo_class_PdoDuckDb_Ext_duckdbLastProfile, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};
