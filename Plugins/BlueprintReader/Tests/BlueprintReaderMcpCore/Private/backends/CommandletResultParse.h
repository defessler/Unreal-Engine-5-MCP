// Parse the asset-registry rows an editor op returns ({"assets":[...]} or
// {"matches":[...]}) into an AssetRegistryListResult. Shared by the commandlet
// and socket backends (the wire shape is identical) and exercisable from doctest
// without a live editor.
#pragma once

#include "backends/IBlueprintReader.h"

#include <nlohmann/json.hpp>
#include <string>

namespace bpr::backends::detail {

inline IBlueprintReader::AssetRegistryListResult
ParseAssetRegistryRows(const nlohmann::json& j, const char* arrayKey) {
	IBlueprintReader::AssetRegistryListResult out;
	if (!j.is_object()) {
		return out;
	}
	auto it = j.find(arrayKey);
	if (it == j.end() || !it->is_array()) {
		return out;
	}
	out.entries.reserve(it->size());
	for (const auto& row : *it) {
		if (!row.is_object()) continue;
		IBlueprintReader::AssetRegistryEntry e;
		e.assetPath = row.value("asset_path", std::string{});
		e.name      = row.value("name",       std::string{});
		e.className = row.value("class_name", std::string{});
		out.entries.push_back(std::move(e));
	}
	return out;
}

}    // namespace bpr::backends::detail
