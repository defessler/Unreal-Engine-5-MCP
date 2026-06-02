// [mock] Validate that read-tool output_schemas match the POPULATED response
// the mock backend returns for a real fixture asset.
//
// Why this exists: the per-mode matrix (test_tool_modes.cpp) dispatches read
// tools against a NON-EXISTENT asset, so it only ever sees an empty/not-found
// result and cannot catch a wrong *populated* shape. The dedicated mock tests
// assert specific fields, not the declared schema. This test closes that gap:
// it pulls each tool's declared output_schema off the descriptor and validates
// a real, populated fixture response against it. Add a row per (tool, args) as
// read tools gain output_schemas.

#include <doctest/doctest.h>

#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"
#include "test_helpers.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {
using nlohmann::json;

bool JsonMatchesType(const json& v, const std::string& t) {
	if (t == "object")  return v.is_object();
	if (t == "array")   return v.is_array();
	if (t == "string")  return v.is_string();
	if (t == "number")  return v.is_number();
	if (t == "integer") return v.is_number_integer();
	if (t == "boolean") return v.is_boolean();
	if (t == "null")    return v.is_null();
	return true;
}

bool TypeAllowed(const json& v, const json& typeNode) {
	if (typeNode.is_string()) {
		return JsonMatchesType(v, typeNode.get<std::string>());
	}
	if (typeNode.is_array()) {
		for (const auto& t : typeNode) {
			if (t.is_string() && JsonMatchesType(v, t.get<std::string>())) {
				return true;
			}
		}
		return false;
	}
	return true;
}

// Shallow JSON-Schema validator (subset): top-level type, object required +
// declared property types, array items. Mirrors the matrix's validator so the
// two stay consistent.
std::string Validate(const json& value, const json& schema) {
	if (!schema.is_object() || schema.empty()) {
		return {};
	}
	if (auto t = schema.find("type"); t != schema.end() && !TypeAllowed(value, *t)) {
		return "top-level type mismatch (declared " + t->dump() + ", got " +
		       std::string(value.type_name()) + ")";
	}
	if (value.is_object()) {
		if (auto req = schema.find("required"); req != schema.end() && req->is_array()) {
			for (const auto& k : *req) {
				if (k.is_string() && !value.contains(k.get<std::string>())) {
					return "missing required key '" + k.get<std::string>() + "'";
				}
			}
		}
		if (auto props = schema.find("properties");
		    props != schema.end() && props->is_object()) {
			for (auto it = props->begin(); it != props->end(); ++it) {
				if (!value.contains(it.key())) {
					continue;
				}
				if (auto pt = it.value().find("type");
				    pt != it.value().end() && !TypeAllowed(value.at(it.key()), *pt)) {
					return "property '" + it.key() + "' type mismatch (declared " +
					       pt->dump() + ")";
				}
			}
		}
	}
	if (value.is_array()) {
		if (auto items = schema.find("items"); items != schema.end() && items->is_object()) {
			for (const auto& el : value) {
				if (std::string r = Validate(el, *items); !r.empty()) {
					return "array element: " + r;
				}
			}
		}
	}
	return {};
}

json OutputSchemaFor(const bpr::tools::ToolRegistry& registry, const std::string& name) {
	for (const auto& d : registry.AllDescriptors()) {
		if (d.name == name) {
			return d.output_schema;
		}
	}
	return json{};
}

}    // namespace

TEST_CASE("[mock] read-tool output_schema matches the populated fixture response") {
	auto reader = bpr::test::MakeMockReader();
	bpr::tools::ToolRegistry registry;
	bpr::tools::RegisterBlueprintTools(registry, reader);

	struct Row {
		std::string tool;
		json        args;
	};
	// One row per read tool that declares an output_schema, pointed at a
	// fixture that exercises a NON-empty result. Grow this as the backfill
	// reaches more read tools.
	const std::vector<Row> rows = {
		{"list_variables",      json{{"asset_path", "/Game/AI/BP_Enemy"}}},
		{"summarize_blueprint", json{{"asset_path", "/Game/AI/BP_Enemy"}}},
	};

	for (const auto& row : rows) {
		CAPTURE(row.tool);
		const auto* fn = registry.Find(row.tool);
		REQUIRE(fn != nullptr);

		const json schema = OutputSchemaFor(registry, row.tool);
		REQUIRE_FALSE(schema.empty());    // the tool must declare an output_schema

		const json result = (*fn)(row.args);
		// Sanity: the chosen fixture must actually populate the result, else
		// the validation is vacuous.
		CHECK_FALSE(result.empty());

		const std::string err = Validate(result, schema);
		CHECK_MESSAGE(err.empty(), row.tool << " populated response violates output_schema: " << err);
	}
}
