#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

struct JevFunctions {
	//! jev_noul, jev_choice, jev_choice_detail, jev_score, jev_score_detail, jev_system_one
	static void RegisterScalarFunctions(ExtensionLoader &loader);
	//! jev_models()
	static void RegisterTableFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
