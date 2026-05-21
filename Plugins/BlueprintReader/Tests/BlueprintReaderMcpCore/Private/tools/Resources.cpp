#include "tools/Resources.h"

#include "backends/IBlueprintReader.h"

#include <fmt/core.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace bpr::tools::resources {

namespace resources_detail {

nlohmann::json DescriptorToJson(const ResourceDescriptor& d) {
	nlohmann::json obj = {
		{"uri",  d.uri},
		{"name", d.name},
	};
	if (!d.description.empty()) {
		obj["description"] = d.description;
	}
	if (!d.mimeType.empty()) {
		obj["mimeType"] = d.mimeType;
	}
	return obj;
}

nlohmann::json ContentsToJson(const ResourceContents& c) {
	nlohmann::json obj = {{"uri", c.uri}};
	if (!c.mimeType.empty()) {
		obj["mimeType"] = c.mimeType;
	}
	if (!c.text.empty()) {
		obj["text"] = c.text;
	}
	if (!c.blob.empty()) {
		obj["blob"] = c.blob;
	}
	return obj;
}

}    // namespace resources_detail
using namespace resources_detail;

void ResourceRegistry::Add(std::unique_ptr<IResourceProvider> provider) {
	if (provider) {
		providers_.push_back(std::move(provider));
	}
}

std::size_t ResourceRegistry::ProviderCount() const {
	return providers_.size();
}

nlohmann::json ResourceRegistry::ListAll() const {
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& p : providers_) {
		for (const auto& d : p->ListResources()) {
			arr.push_back(DescriptorToJson(d));
		}
	}
	return arr;
}

nlohmann::json ResourceRegistry::Read(std::string_view uri) {
	for (auto& p : providers_) {
		if (p->Handles(uri)) {
			ResourceContents body = p->ReadResource(uri);
			return nlohmann::json{
				{"contents", nlohmann::json::array({ContentsToJson(body)})},
			};
		}
	}
	throw std::invalid_argument(fmt::format(
		"no resource provider handles URI '{}' — known schemes: bp:///Game/..., "
		"bp:///_project, bp:///_output_log", std::string(uri)));
}

bool ResourceRegistry::Handles(std::string_view uri) const {
	for (const auto& p : providers_) {
		if (p->Handles(uri)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------
// Built-in providers
// ---------------------------------------------------------------------

namespace builtin_providers {

// Common helpers --------------------------------------------------------
constexpr const char* kSchemePrefix = "bp:///";

// Strip the `bp:///` scheme prefix; returns the resource-local path.
// Empty string when the URI doesn't have the prefix.
std::string StripScheme(std::string_view uri) {
	const std::string_view sv(kSchemePrefix);
	if (uri.size() < sv.size()) {
		return {};
	}
	if (uri.substr(0, sv.size()) != sv) {
		return {};
	}
	return std::string(uri.substr(sv.size()));
}

// Blueprint asset provider — `bp:///Game/AI/BP_Foo` → ReadBlueprint
// result as a JSON text blob. The agent gets the same metadata
// `read_blueprint` would emit, but addressable as a stable URI.
class BlueprintAssetProvider : public IResourceProvider {
public:
	explicit BlueprintAssetProvider(backends::IBlueprintReader& reader)
		: reader_(reader) {}

	bool Handles(std::string_view uri) const override {
		const auto rest = StripScheme(uri);
		// `Game/AI/BP_Foo` — note we strip the leading `/` that the
		// bp:///Game form puts at the start of `rest`. Hmm — actually
		// `bp:///Game/AI/BP_Foo` has the prefix `bp:///` and rest
		// `Game/AI/BP_Foo`. We're checking for the "Game/" namespace.
		if (rest.empty() || rest.front() == '_') {
			return false;  // _project, _output_log live elsewhere
		}
		return rest.size() >= 5 && rest.substr(0, 5) == "Game/";
	}

	std::vector<ResourceDescriptor> ListResources() override {
		// Enumerate `/Game` via the reader. On backends that can't list
		// (mock without fixtures of this kind, or a misconfigured live
		// editor), surface an empty list — the agent can still
		// resources/read a URI directly without it appearing here.
		std::vector<ResourceDescriptor> out;
		try {
			auto bps = reader_.ListBlueprints("/Game");
			out.reserve(bps.size());
			for (const auto& s : bps) {
				ResourceDescriptor d;
				// `s.AssetPath` is `/Game/...` — drop the leading `/`
				// for the bp:///Game/... shape.
				const std::string body = s.AssetPath.empty() || s.AssetPath[0] != '/'
											  ? s.AssetPath
											  : s.AssetPath.substr(1);
				d.uri      = std::string(kSchemePrefix) + body;
				d.name     = s.Name;
				d.mimeType = "application/json";
				d.description = "Blueprint metadata (parent class, variables, "
								"function summaries) for " + s.AssetPath;
				out.push_back(std::move(d));
			}
		} catch (...) {
			// Backend can't list — return empty. Reads will still work
			// against well-known asset paths.
		}
		return out;
	}

	ResourceContents ReadResource(std::string_view uri) override {
		const auto rest = StripScheme(uri);
		if (rest.empty() || rest.substr(0, 4) != "Game") {
			throw std::invalid_argument(fmt::format(
				"BlueprintAssetProvider can't handle URI '{}'", std::string(uri)));
		}
		// Reconstruct the canonical /Game/... asset path.
		const std::string assetPath = "/" + rest;
		BPMetadata meta = reader_.ReadBlueprint(assetPath);
		ResourceContents c;
		c.uri      = std::string(uri);
		c.mimeType = "application/json";
		c.text     = nlohmann::json(meta).dump(2);
		return c;
	}

private:
	backends::IBlueprintReader& reader_;
};

// Project metadata provider — `bp:///_project` → .uproject summary
// as JSON.
class ProjectMetadataProvider : public IResourceProvider {
public:
	explicit ProjectMetadataProvider(backends::IBlueprintReader& reader)
		: reader_(reader) {}

	bool Handles(std::string_view uri) const override {
		return uri == "bp:///_project";
	}

	std::vector<ResourceDescriptor> ListResources() override {
		ResourceDescriptor d;
		d.uri         = "bp:///_project";
		d.name        = "Project metadata";
		d.mimeType    = "application/json";
		d.description = ".uproject summary: engine association, category, "
						"description, plus the raw JSON body.";
		return {d};
	}

	ResourceContents ReadResource(std::string_view uri) override {
		if (!Handles(uri)) {
			throw std::invalid_argument(fmt::format(
				"ProjectMetadataProvider can't handle URI '{}'", std::string(uri)));
		}
		auto m = reader_.GetProjectMetadata();
		nlohmann::json body = {
			{"project_name",        m.projectName},
			{"project_path",        m.projectPath},
			{"engine_association",  m.engineAssociation},
			{"category",            m.category},
			{"description",         m.description},
			{"raw",                 m.raw},
		};
		ResourceContents c;
		c.uri      = std::string(uri);
		c.mimeType = "application/json";
		c.text     = body.dump(2);
		return c;
	}

private:
	backends::IBlueprintReader& reader_;
};

// Output log provider — `bp:///_output_log` → editor's recent log
// tail. Best-effort: backends that don't support ReadOutputLog throw
// on read, which surfaces as a clean resource-not-found.
class OutputLogProvider : public IResourceProvider {
public:
	explicit OutputLogProvider(backends::IBlueprintReader& reader)
		: reader_(reader) {}

	bool Handles(std::string_view uri) const override {
		return uri == "bp:///_output_log";
	}

	std::vector<ResourceDescriptor> ListResources() override {
		ResourceDescriptor d;
		d.uri         = "bp:///_output_log";
		d.name        = "Editor output log";
		d.mimeType    = "text/plain";
		d.description = "Recent UE editor output log lines (tail). "
						"Requires a live editor; commandlet-only sessions "
						"return an error.";
		return {d};
	}

	ResourceContents ReadResource(std::string_view uri) override {
		if (!Handles(uri)) {
			throw std::invalid_argument(fmt::format(
				"OutputLogProvider can't handle URI '{}'", std::string(uri)));
		}
		auto log = reader_.ReadOutputLog(/*limit=*/256, /*minSeverity=*/{});
		std::string text;
		for (const auto& e : log.entries) {
			if (!e.timestamp.empty()) {
				text += e.timestamp;
				text += ' ';
			}
			text += '[';
			text += e.severity.empty() ? "Log" : e.severity;
			text += "] ";
			if (!e.category.empty()) {
				text += e.category;
				text += ": ";
			}
			text += e.message;
			text += '\n';
		}
		ResourceContents c;
		c.uri      = std::string(uri);
		c.mimeType = "text/plain";
		c.text     = text.empty() ? std::string("<empty>") : std::move(text);
		return c;
	}

private:
	backends::IBlueprintReader& reader_;
};

}    // namespace builtin_providers

std::unique_ptr<IResourceProvider> MakeBlueprintAssetProvider(
	backends::IBlueprintReader& reader) {
	return std::make_unique<builtin_providers::BlueprintAssetProvider>(reader);
}

std::unique_ptr<IResourceProvider> MakeProjectMetadataProvider(
	backends::IBlueprintReader& reader) {
	return std::make_unique<builtin_providers::ProjectMetadataProvider>(reader);
}

std::unique_ptr<IResourceProvider> MakeOutputLogProvider(
	backends::IBlueprintReader& reader) {
	return std::make_unique<builtin_providers::OutputLogProvider>(reader);
}

}    // namespace bpr::tools::resources
