# Chapter 2 — A mock backend and the tool registry

In chapter 1 you got an empty MCP server talking JSON-RPC. Now you'll
give it something to do: a `list_blueprints` tool that lists fake
blueprint assets from on-disk JSON fixtures, and a `read_blueprint`
tool that returns one fixture's metadata. No Unreal yet — that arrives
in chapter 3. The point of this chapter is to nail down two
abstractions you'll keep for the life of the server:

1. **`IBlueprintReader`** — a backend interface. Concrete subclasses
   (mock today, commandlet and live in later chapters) all implement
   the same shape, so the tool layer never knows whether it's reading
   fixtures or driving a real editor.
2. **`ToolRegistry`** — a name → handler map plus JSON-Schema
   descriptors, so MCP `tools/list` is just a serialization of the
   registry and `tools/call` is a name lookup.

By the end of this chapter, `tools/list` reports two tools and
`tools/call list_blueprints` returns real fixture data.

## The wire types

Before you can define an interface, you need the types it returns. Keep
these in a single header — every backend, every tool handler, and every
test will include it. The production version is
`Plugins/BlueprintReader/mcp-server/src/BlueprintReaderTypes.h`; here's
the minimum slice for chapter 2.

`src/BlueprintReaderTypes.h`:

```cpp
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace bpr {

struct BPAssetSummary {
    std::string AssetPath;     // "/Game/AI/BP_Enemy"
    std::string Name;          // "BP_Enemy"
    std::string ParentClass;   // "ACharacter"
    std::string ModifiedIso;   // ISO-8601 timestamp
};

struct BPPinType {
    std::string Category;          // "real", "object", "bool", "string"...
    std::string SubCategory;       // "float" when Category="real"; else empty
    std::string SubCategoryObject; // e.g. "Actor" for object pins; else empty
    bool IsArray = false;
    bool IsSet   = false;
    bool IsMap   = false;
};

struct BPVariable {
    std::string Name;
    BPPinType   Type;
    std::string DefaultValue;
    std::string Category;
    bool IsReplicated = false;
    bool IsEditable   = false;
};

struct BPMetadata {
    std::string AssetPath;
    std::string Name;
    std::string ParentClass;
    std::vector<std::string> Interfaces;
    std::vector<BPVariable>  Variables;
    std::vector<std::string> FunctionNames;
    std::vector<std::string> GraphNames;
};

// snake_case JSON serializers — nlohmann::json picks these up via ADL.
// The wire format is snake_case; C++ field names stay PascalCase.
// Crossing the boundary is one function per type.

inline void to_json(nlohmann::json& j, const BPPinType& t) {
    auto strOrNull = [](const std::string& s) {
        return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
    };
    j = {{"category", t.Category},
         {"sub_category", strOrNull(t.SubCategory)},
         {"sub_category_object", strOrNull(t.SubCategoryObject)},
         {"is_array", t.IsArray}, {"is_set", t.IsSet}, {"is_map", t.IsMap}};
}
inline void from_json(const nlohmann::json& j, BPPinType& t) {
    t.Category = j.at("category").get<std::string>();
    if (auto it = j.find("sub_category"); it != j.end() && it->is_string())
        t.SubCategory = it->get<std::string>();
    if (auto it = j.find("sub_category_object"); it != j.end() && it->is_string())
        t.SubCategoryObject = it->get<std::string>();
    t.IsArray = j.value("is_array", false);
    t.IsSet   = j.value("is_set", false);
    t.IsMap   = j.value("is_map", false);
}

inline void to_json(nlohmann::json& j, const BPVariable& v) {
    auto strOrNull = [](const std::string& s) {
        return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
    };
    j = {{"name", v.Name}, {"type", v.Type},
         {"default_value", strOrNull(v.DefaultValue)},
         {"category",      strOrNull(v.Category)},
         {"is_replicated", v.IsReplicated},
         {"is_editable",   v.IsEditable}};
}
inline void from_json(const nlohmann::json& j, BPVariable& v) {
    v.Name = j.at("name").get<std::string>();
    v.Type = j.at("type").get<BPPinType>();
    if (auto it = j.find("default_value"); it != j.end() && it->is_string())
        v.DefaultValue = it->get<std::string>();
    if (auto it = j.find("category"); it != j.end() && it->is_string())
        v.Category = it->get<std::string>();
    v.IsReplicated = j.value("is_replicated", false);
    v.IsEditable   = j.value("is_editable", false);
}

inline void to_json(nlohmann::json& j, const BPAssetSummary& s) {
    j = {{"asset_path", s.AssetPath}, {"name", s.Name},
         {"parent_class", s.ParentClass}, {"modified_iso", s.ModifiedIso}};
}
inline void from_json(const nlohmann::json& j, BPAssetSummary& s) {
    s.AssetPath   = j.at("asset_path").get<std::string>();
    s.Name        = j.value("name", "");
    s.ParentClass = j.value("parent_class", "");
    s.ModifiedIso = j.value("modified_iso", "");
}

inline void to_json(nlohmann::json& j, const BPMetadata& m) {
    j = {{"asset_path", m.AssetPath}, {"name", m.Name},
         {"parent_class", m.ParentClass}, {"interfaces", m.Interfaces},
         {"variables", m.Variables},
         {"functions", nlohmann::json::array()},
         {"graphs",    nlohmann::json::array()}};
    for (const auto& n : m.FunctionNames) j["functions"].push_back({{"name", n}});
    for (const auto& n : m.GraphNames)    j["graphs"].push_back({{"name", n}});
}
inline void from_json(const nlohmann::json& j, BPMetadata& m) {
    m.AssetPath   = j.at("asset_path").get<std::string>();
    m.Name        = j.value("name", "");
    m.ParentClass = j.value("parent_class", "");
    if (auto it = j.find("interfaces"); it != j.end())
        m.Interfaces = it->get<std::vector<std::string>>();
    if (auto it = j.find("variables"); it != j.end())
        m.Variables = it->get<std::vector<BPVariable>>();
    if (auto it = j.find("functions"); it != j.end())
        for (const auto& f : *it) m.FunctionNames.push_back(f.at("name"));
    if (auto it = j.find("graphs"); it != j.end())
        for (const auto& g : *it) m.GraphNames.push_back(g.at("name"));
}

} // namespace bpr
```

Two style notes worth internalizing now:

- **`null` for missing strings**, not `""`. Some agent clients sniff
  field types and treat empty strings as "present but blank" —
  different from "absent". Rationale in `../design/06-wire-protocol.md`.
- **`BPPinType` is a nested object**, not a stringly-typed shorthand.
  Shorthand layer arrives later — see
  `Plugins/BlueprintReader/mcp-server/src/tools/TypeShorthand.cpp`.

## The backend interface

`src/backends/IBlueprintReader.h`:

```cpp
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "BlueprintReaderTypes.h"

namespace bpr::backends {

class BlueprintReaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AssetNotFound : public BlueprintReaderError {
public:
    using BlueprintReaderError::BlueprintReaderError;
};

class IBlueprintReader {
public:
    virtual ~IBlueprintReader() = default;

    virtual std::vector<BPAssetSummary>
        ListBlueprints(std::string_view path) = 0;

    virtual BPMetadata
        ReadBlueprint(std::string_view assetPath) = 0;
};

} // namespace bpr::backends
```

Two methods is enough for the chapter. The real interface (in
`Plugins/BlueprintReader/mcp-server/src/backends/IBlueprintReader.h`) has
~80 read and write methods by the time it covers blueprints + data
tables + materials + automation — but they're all the same pattern.
Add one when you need it.

## The mock backend

`src/backends/MockBlueprintReader.h`:

```cpp
#pragma once
#include <filesystem>
#include <unordered_map>
#include "backends/IBlueprintReader.h"

namespace bpr::backends {

class MockBlueprintReader final : public IBlueprintReader {
public:
    explicit MockBlueprintReader(const std::filesystem::path& fixturesDir);

    std::vector<BPAssetSummary>
        ListBlueprints(std::string_view path) override;
    BPMetadata
        ReadBlueprint(std::string_view assetPath) override;

private:
    struct FixtureEntry {
        BPAssetSummary summary;
        BPMetadata     metadata;
    };
    void LoadFile(const std::filesystem::path& file);
    const FixtureEntry& Require(std::string_view assetPath) const;

    std::unordered_map<std::string, FixtureEntry> assets_;
};

} // namespace bpr::backends
```

`src/backends/MockBlueprintReader.cpp`:

```cpp
#include "backends/MockBlueprintReader.h"
#include <fstream>
#include <fmt/core.h>

namespace bpr::backends {

MockBlueprintReader::MockBlueprintReader(const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir)) {
        throw BlueprintReaderError(
            fmt::format("not a directory: {}", dir.string()));
    }
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            LoadFile(e.path());
        }
    }
}

void MockBlueprintReader::LoadFile(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw BlueprintReaderError(
        fmt::format("cannot open: {}", file.string()));
    nlohmann::json j;  in >> j;

    FixtureEntry entry;
    entry.summary  = j.at("summary").get<BPAssetSummary>();
    entry.metadata = j.at("metadata").get<BPMetadata>();
    if (entry.summary.AssetPath.empty())
        throw BlueprintReaderError("fixture missing asset_path");
    assets_.emplace(entry.summary.AssetPath, std::move(entry));
}

const MockBlueprintReader::FixtureEntry&
MockBlueprintReader::Require(std::string_view assetPath) const {
    auto it = assets_.find(std::string(assetPath));
    if (it == assets_.end())
        throw AssetNotFound(fmt::format("asset not found: {}", assetPath));
    return it->second;
}

std::vector<BPAssetSummary>
MockBlueprintReader::ListBlueprints(std::string_view path) {
    std::vector<BPAssetSummary> out;
    for (const auto& [p, e] : assets_) {
        const auto& ap = e.summary.AssetPath;
        if (path.empty() || path == "/Game" ||
            (ap.size() >= path.size() && ap.compare(0, path.size(), path) == 0))
        {
            out.push_back(e.summary);
        }
    }
    return out;
}

BPMetadata MockBlueprintReader::ReadBlueprint(std::string_view assetPath) {
    return Require(assetPath).metadata;
}

} // namespace bpr::backends
```

## Fixture JSON

Drop these in `fixtures/`. The format mirrors the wire shape — each
fixture is one file per BP, top-level keys for the slices each tool
returns. The full version in
`Plugins/BlueprintReader/mcp-server/fixtures/BP_Enemy.json` adds graphs
and functions; chapter 2 only needs summary + metadata.

`fixtures/BP_Enemy.json`:

```json
{
  "summary": {
    "asset_path": "/Game/AI/BP_Enemy",
    "name": "BP_Enemy",
    "parent_class": "ACharacter",
    "modified_iso": "2026-04-22T18:14:03Z"
  },
  "metadata": {
    "asset_path": "/Game/AI/BP_Enemy",
    "name": "BP_Enemy",
    "parent_class": "ACharacter",
    "interfaces": [],
    "variables": [
      {
        "name": "Health",
        "type": {"category":"real","sub_category":"float",
                 "sub_category_object":null,
                 "is_array":false,"is_set":false,"is_map":false},
        "default_value": "100.0",
        "category": "Combat",
        "is_replicated": true,
        "is_editable": true
      }
    ],
    "functions": [{"name":"TakeDamage"}, {"name":"OnDeath"}],
    "graphs":    [{"name":"EventGraph"}]
  }
}
```

Add a second fixture (`fixtures/BP_Pickup.json`) so `list_blueprints`
has more than one row to return. Use the same shape with a different
asset path.

Have CMake stage `fixtures/` next to the executable so the default
fixture-loading path (cwd-relative or exe-adjacent) works after build:

```cmake
add_custom_command(TARGET bp-reader-mcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        "$<TARGET_FILE_DIR:bp-reader-mcp>/fixtures")
```

## The tool registry

`src/tools/ToolRegistry.h`:

```cpp
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace bpr::tools {

using ToolFn = std::function<nlohmann::json(const nlohmann::json& args)>;

struct ToolDescriptor {
    std::string name;
    std::string description;
    nlohmann::json input_schema;  // JSON-Schema-shaped
};

class ToolRegistry {
public:
    void Add(ToolDescriptor d, ToolFn fn);

    // Serialize every descriptor for tools/list.
    nlohmann::json ListSpec() const;
    const ToolFn* Find(const std::string& name) const;

private:
    std::vector<ToolDescriptor> specs_;
    std::unordered_map<std::string, ToolFn> fns_;
};

} // namespace bpr::tools
```

`src/tools/ToolRegistry.cpp`:

```cpp
#include "tools/ToolRegistry.h"

namespace bpr::tools {

void ToolRegistry::Add(ToolDescriptor d, ToolFn fn) {
    fns_[d.name] = std::move(fn);
    specs_.push_back(std::move(d));
}

nlohmann::json ToolRegistry::ListSpec() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : specs_) {
        arr.push_back({
            {"name",        s.name},
            {"description", s.description},
            {"inputSchema", s.input_schema},
        });
    }
    return arr;
}

const ToolFn* ToolRegistry::Find(const std::string& name) const {
    auto it = fns_.find(name);
    return it == fns_.end() ? nullptr : &it->second;
}

} // namespace bpr::tools
```

## Registering the two tools

`src/tools/BlueprintTools.cpp`:

```cpp
#include "tools/BlueprintTools.h"
#include "backends/IBlueprintReader.h"
#include <fmt/core.h>

namespace bpr::tools {

void RegisterBlueprintTools(ToolRegistry& reg,
                            backends::IBlueprintReader& reader) {
    // ---- list_blueprints --------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "list_blueprints";
        d.description = "List BP assets under a content path. Defaults to /Game.";
        d.input_schema = {
            {"type","object"},
            {"properties", {{"path", {{"type","string"}}}}},
        };
        reg.Add(std::move(d), [&reader](const nlohmann::json& args) {
            std::string path = args.value("path", "/Game");
            return nlohmann::json(reader.ListBlueprints(path));
        });
    }

    // ---- read_blueprint ---------------------------------------------------
    {
        ToolDescriptor d;
        d.name = "read_blueprint";
        d.description = "Read top-level metadata for a blueprint.";
        d.input_schema = {
            {"type","object"},
            {"properties", {{"asset_path", {{"type","string"}}}}},
            {"required", nlohmann::json::array({"asset_path"})},
        };
        reg.Add(std::move(d), [&reader](const nlohmann::json& args) {
            if (!args.contains("asset_path") || !args["asset_path"].is_string())
                throw std::invalid_argument("missing string asset_path");
            return nlohmann::json(
                reader.ReadBlueprint(args["asset_path"].get<std::string>()));
        });
    }
}

} // namespace bpr::tools
```

For the full, production version with `fields`/`limit`/`offset` response
controls, see
`Plugins/BlueprintReader/mcp-server/src/tools/BlueprintTools.cpp`. Those
are quality-of-life additions you can layer on later.

## Wiring tools/list and tools/call into main.cpp

Update `src/main.cpp` to register the tools and surface them on the
right methods:

```cpp
#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Server.h"
#include "tools/ToolRegistry.h"
#include "tools/BlueprintTools.h"

#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    fs::path fixtures = fs::current_path() / "fixtures";

    bpr::backends::MockBlueprintReader reader(fixtures);
    bpr::tools::ToolRegistry registry;
    bpr::tools::RegisterBlueprintTools(registry, reader);

    bpr::jsonrpc::Server server;

    server.Register("initialize", [](const nlohmann::json&) {
        return bpr::jsonrpc::Response::Ok({
            {"protocolVersion","2024-11-05"},
            {"capabilities", {{"tools", nlohmann::json::object()}}},
            {"serverInfo",   {{"name","bp-reader-mcp"},{"version","0.1.0"}}},
        });
    });
    server.Register("notifications/initialized",
        [](const nlohmann::json&) {
            return bpr::jsonrpc::Response::Ok(nlohmann::json::object());
        });
    server.Register("ping", [](const nlohmann::json&) {
        return bpr::jsonrpc::Response::Ok(nlohmann::json::object());
    });

    server.Register("tools/list", [&registry](const nlohmann::json&) {
        return bpr::jsonrpc::Response::Ok({{"tools", registry.ListSpec()}});
    });

    server.Register("tools/call",
        [&registry](const nlohmann::json& params) {
            std::string name = params.at("name").get<std::string>();
            nlohmann::json args = params.value("arguments",
                                              nlohmann::json::object());
            auto* fn = registry.Find(name);
            if (!fn) {
                return bpr::jsonrpc::Response::Ok({
                    {"isError", true},
                    {"content", nlohmann::json::array({
                        {{"type","text"},
                         {"text", "unknown tool: " + name}}})},
                });
            }
            try {
                nlohmann::json result = (*fn)(args);
                return bpr::jsonrpc::Response::Ok({
                    {"isError", false},
                    {"content", nlohmann::json::array({
                        {{"type","text"},
                         {"text", result.dump(2)}}})},
                });
            } catch (const std::exception& e) {
                return bpr::jsonrpc::Response::Ok({
                    {"isError", true},
                    {"content", nlohmann::json::array({
                        {{"type","text"},
                         {"text", std::string("tool error: ") + e.what()}}})},
                });
            }
        });

    server.Run(std::cin, std::cout, std::cerr);
    return 0;
}
```

Note the `tools/call` response shape: MCP wraps every tool result in a
`{content: [{type: "text", text: "..."}], isError: bool}` envelope. The
*inner* text is the canonical JSON your tool actually returns; the
outer envelope is fixed by spec. Production code adds `_meta` with
timing and arg context — see
`Plugins/BlueprintReader/mcp-server/src/jsonrpc/Mcp.cpp`.

Also notice: tool errors come back as `isError: true` results, not as
JSON-RPC `error` envelopes. That's deliberate — a missing tool isn't a
protocol fault, it's a tool-level failure the client should surface
inline. The JSON-RPC `error` envelope is reserved for malformed
requests, parse errors, and unknown *methods*.

## A doctest round-trip

`tests/test_server.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include <filesystem>

TEST_CASE("list_blueprints + read_blueprint round-trip against fixtures") {
    // Locate fixtures relative to the test exe (CMake staged them).
    auto here = std::filesystem::path(__FILE__).parent_path();
    auto fixtures = here.parent_path() / "fixtures";

    bpr::backends::MockBlueprintReader reader(fixtures);
    bpr::tools::ToolRegistry reg;
    bpr::tools::RegisterBlueprintTools(reg, reader);

    auto* listFn = reg.Find("list_blueprints");
    REQUIRE(listFn);
    auto listed = (*listFn)({{"path","/Game/AI"}});
    REQUIRE(listed.is_array());
    REQUIRE(listed.size() >= 1);

    auto* readFn = reg.Find("read_blueprint");
    REQUIRE(readFn);
    auto meta = (*readFn)({{"asset_path","/Game/AI/BP_Enemy"}});
    CHECK(meta.at("name") == "BP_Enemy");
    CHECK(meta.at("parent_class") == "ACharacter");
    CHECK(meta.at("variables").at(0).at("name") == "Health");
}
```

Update `tests/CMakeLists.txt` to depend on the staged fixtures and to
link the registry:

```cmake
add_executable(bp-reader-tests test_server.cpp)
target_link_libraries(bp-reader-tests PRIVATE bp-reader-core doctest)
add_test(NAME bp-reader-tests COMMAND bp-reader-tests)
```

Build and run:

```pwsh
cmake --build build --config Release
.\build\tests\Release\bp-reader-tests.exe
```

You should see a single test passing.

## Checkpoint

Drive the server end-to-end:

```pwsh
@'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_blueprints","arguments":{"path":"/Game"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_blueprint","arguments":{"asset_path":"/Game/AI/BP_Enemy"}}}
'@ | .\build\Release\bp-reader-mcp.exe
```

Expect:

- The `tools/list` response contains two entries with names
  `list_blueprints` and `read_blueprint`, each with a non-empty
  `inputSchema`.
- The first `tools/call` response has `isError: false` and the inner
  `text` field deserializes to a JSON array with `BP_Enemy` and
  `BP_Pickup` summaries.
- The second `tools/call` returns the full metadata for `BP_Enemy`
  with a `Health` variable.

Common failure modes:

- **`not a directory: fixtures`** — you launched the exe from a
  directory that doesn't have `fixtures/` next to it. Either `cd` into
  `build/Release/` or pass an absolute path. Production code reads
  `BP_READER_FIXTURES_DIR` from env; bolt that on whenever you like.
- **`unknown tool: list_blueprints`** — `RegisterBlueprintTools` never
  ran. Check that `main.cpp` calls it before `server.Run`.
- **`tools/list` returns an empty array** — the registry was created
  but never populated. Same fix as above.
- **The tool result `text` looks like `"\"name\": \"BP_Enemy\""` with
  escaped quotes** — that's normal. The outer envelope holds JSON-as-
  text. Parse the `text` field again to get back the structured shape.

You now have a working MCP server that talks to a pluggable backend.
Chapter 3 swaps the mock for a real one: an Unreal editor commandlet
that reads `.uasset` files off disk.
