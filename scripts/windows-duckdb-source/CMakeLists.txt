cmake_minimum_required(VERSION 3.24)
project(PdoDuckDBWindowsBridge LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The combined DuckDB source package requires x64 MSVC")
endif()
foreach(required DUCKDB_ENGINE_SOURCE PDO_DUCKDB_BRIDGE_SOURCE PDO_DUCKDB_BRIDGE_CACHE PDO_DUCKDB_PACKAGE_OUTPUT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(BUILD_UNITTESTS OFF CACHE BOOL "Build DuckDB tests" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "Build DuckDB shell" FORCE)
set(BUILD_EXTENSIONS "autocomplete;icu;json;tpcds;tpch" CACHE STRING "Bundled DuckDB extensions" FORCE)
add_subdirectory("${DUCKDB_ENGINE_SOURCE}" engine)

file(GLOB bridge_sources CONFIGURE_DEPENDS "${PDO_DUCKDB_BRIDGE_SOURCE}/pdo_duckdb_bridge*.cpp")
file(GLOB bridge_headers CONFIGURE_DEPENDS "${PDO_DUCKDB_BRIDGE_SOURCE}/pdo_duckdb_bridge*.h")
if(NOT bridge_sources OR NOT bridge_headers)
  message(FATAL_ERROR "Missing DuckDB bridge sources or public headers")
endif()
set(bridge_exports "")
foreach(header IN LISTS bridge_headers)
  file(READ "${header}" contents)
  string(REGEX MATCHALL "pdd_bridge_[A-Za-z0-9_]+[ \t\r\n]*\\(" declarations "${contents}")
  foreach(declaration IN LISTS declarations)
    string(REGEX REPLACE "[ \t\r\n]*\\($" "" symbol "${declaration}")
    list(APPEND bridge_exports "${symbol}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES bridge_exports)
list(SORT bridge_exports)
if(NOT "pdd_bridge_check" IN_LIST bridge_exports OR NOT "pdd_bridge_value_render" IN_LIST bridge_exports)
  message(FATAL_ERROR "The bridge export declarations are incomplete")
endif()
string(REPLACE ";" "\n" export_lines "${bridge_exports}")
set(definition "${CMAKE_CURRENT_BINARY_DIR}/duckdb-bridge.def")
file(WRITE "${definition}" "LIBRARY duckdb\nEXPORTS\n${export_lines}\n")

target_sources(duckdb PRIVATE ${bridge_sources} "${definition}")
set_source_files_properties(${bridge_sources} TARGET_DIRECTORY duckdb
  PROPERTIES COMPILE_DEFINITIONS DUCKDB_STATIC_BUILD)
target_include_directories(duckdb PRIVATE "${PDO_DUCKDB_BRIDGE_SOURCE}" "${PDO_DUCKDB_BRIDGE_CACHE}/include")
target_compile_options(duckdb PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/bigobj;/Zc:__cplusplus;/DNOMINMAX>")
set_target_properties(duckdb PROPERTIES
  CXX_STANDARD 17
  MSVC_RUNTIME_LIBRARY MultiThreaded
  RUNTIME_OUTPUT_DIRECTORY "${PDO_DUCKDB_PACKAGE_OUTPUT}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PDO_DUCKDB_PACKAGE_OUTPUT}"
  ARCHIVE_OUTPUT_DIRECTORY "${PDO_DUCKDB_PACKAGE_OUTPUT}/lib"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${PDO_DUCKDB_PACKAGE_OUTPUT}/lib")
