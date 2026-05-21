// MCP Resources primitive — URI-addressable assets the client can list +
// read independently of the tool-call path. Spec:
// https://modelcontextprotocol.io/specification/2025-06-18/server/resources
//
// We use the `bp://` URI scheme:
//   - bp:///Game/...               — Blueprint asset reader (text/json)
//   - bp:///_project               — project metadata (text/json)
//   - bp:///_output_log            — UE editor output log (text/plain)
//
// Each scheme is served by an IResourceProvider. The ResourceRegistry
// holds the providers and dispatches list/read requests to whichever
// claims the URI. Providers are stateless w.r.t. the registry —
// implementations may hold their own state (the BP provider holds an
// IBlueprintReader&, for instance).
//
// resources/list response: array of {uri, name, description?, mimeType?}
// resources/read response: {contents: [{uri, mimeType?, text|blob}]}
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::backends {
class IBlueprintReader;
}

namespace bpr::tools::resources {

// A descriptor emitted by a provider's ListResources call. Mirrors the
// MCP wire shape — `description` and `mimeType` are optional. Empty
// fields aren't serialised.
struct ResourceDescriptor {
	std::string uri;
	std::string name;
	std::string description;
	std::string mimeType;
};

// resources/read response body. `text` is set for text MIME types;
// `blob` (base64) is set for binary. Exactly one of the two should be
// non-empty per spec.
struct ResourceContents {
	std::string uri;
	std::string mimeType;
	std::string text;    // populated for text/* MIME types
	std::string blob;    // base64; populated for binary
};

// A provider for one URI namespace. Implementations decide whether
// the URI matches by inspecting its prefix or shape (e.g. the BP
// reader provider handles bp:///Game/...).
class IResourceProvider {
public:
	virtual ~IResourceProvider() = default;

	// True when this provider can serve the URI. Cheap inspection — no
	// I/O. Used by the registry to route Read calls.
	virtual bool Handles(std::string_view uri) const = 0;

	// Enumerate the resources this provider offers. The registry calls
	// every provider's ListResources and concatenates the results.
	// May be empty (e.g. an output-log provider only serves on demand,
	// no listing).
	virtual std::vector<ResourceDescriptor> ListResources() = 0;

	// Read the named resource. Throws std::invalid_argument when the
	// URI doesn't match this provider's namespace. Returns the
	// ResourceContents body with exactly one of `text` / `blob` set.
	// The dispatcher converts std::invalid_argument to ResourceNotFound.
	virtual ResourceContents ReadResource(std::string_view uri) = 0;
};

// Aggregate registry. Holds owned providers; resources/list dispatches
// across all of them, resources/read finds the first provider whose
// Handles() returns true.
class ResourceRegistry {
public:
	// Take ownership of a provider. Registration order = list order.
	void Add(std::unique_ptr<IResourceProvider> provider);

	// Number of registered providers (for diagnostics).
	std::size_t ProviderCount() const;

	// resources/list — array of ResourceDescriptor (JSON shape).
	nlohmann::json ListAll() const;

	// resources/read — returns {contents: [...]} body. Throws
	// std::invalid_argument when no provider handles the URI;
	// the dispatcher converts that to a JSON-RPC ResourceNotFound.
	nlohmann::json Read(std::string_view uri);

	// True when at least one provider declares this URI handle-able.
	// Used by the dispatcher to distinguish "unknown URI" from "I tried
	// to read it but the asset was missing" — both surface as
	// resource-not-found per spec, but the message can be more
	// specific in the latter case.
	bool Handles(std::string_view uri) const;

private:
	std::vector<std::unique_ptr<IResourceProvider>> providers_;
};

// ---------- Built-in providers ----------------------------------------

// Builds the three bp:// providers documented above. `reader` must
// outlive the registry. `uprojectPath` is optional — when empty, the
// project-metadata provider's ListResources still advertises the
// resource but ReadResource will surface an error.
std::unique_ptr<IResourceProvider> MakeBlueprintAssetProvider(
	backends::IBlueprintReader& reader);

std::unique_ptr<IResourceProvider> MakeProjectMetadataProvider(
	backends::IBlueprintReader& reader);

std::unique_ptr<IResourceProvider> MakeOutputLogProvider(
	backends::IBlueprintReader& reader);

}    // namespace bpr::tools::resources
