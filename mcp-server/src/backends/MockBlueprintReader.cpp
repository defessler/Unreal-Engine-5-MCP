#include "backends/MockBlueprintReader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace bpr::backends {

namespace {

std::string LowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool ContainsCI(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto h = LowerAscii(haystack);
    auto n = LowerAscii(needle);
    return h.find(n) != std::string::npos;
}

// Path matching for ListBlueprints: prefix match on the fixture's asset_path.
// `/Game` matches everything under /Game; `/Game/AI` matches only that subtree.
bool PathMatches(std::string_view filter, std::string_view assetPath) {
    if (filter.empty() || filter == "/" || filter == "/Game") {
        // Root filter — anything under /Game counts.
        return assetPath.rfind("/Game", 0) == 0;
    }
    if (assetPath.size() < filter.size()) return false;
    if (assetPath.compare(0, filter.size(), filter) != 0) return false;
    if (assetPath.size() == filter.size()) return true;
    char next = assetPath[filter.size()];
    return next == '/';
}

} // namespace

MockBlueprintReader::MockBlueprintReader(const std::filesystem::path& fixturesDir) {
    if (!std::filesystem::exists(fixturesDir)) {
        throw BlueprintReaderError(
            fmt::format("fixture directory does not exist: {}", fixturesDir.string()));
    }
    if (!std::filesystem::is_directory(fixturesDir)) {
        throw BlueprintReaderError(
            fmt::format("fixture path is not a directory: {}", fixturesDir.string()));
    }
    LoadDir(fixturesDir);
}

void MockBlueprintReader::LoadDir(const std::filesystem::path& dir) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        LoadFile(entry.path());
    }
}

void MockBlueprintReader::LoadFile(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        throw BlueprintReaderError(fmt::format("failed to open fixture: {}", file.string()));
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        throw BlueprintReaderError(
            fmt::format("failed to parse fixture {}: {}", file.string(), e.what()));
    }

    FixtureEntry entry;
    try {
        entry.summary = j.at("summary").get<BPAssetSummary>();
        entry.metadata = j.at("metadata").get<BPMetadata>();
        if (auto graphsIt = j.find("graphs"); graphsIt != j.end()) {
            entry.graphs = graphsIt->get<std::vector<BPGraph>>();
        }
        if (auto fnIt = j.find("functions"); fnIt != j.end()) {
            entry.functions = fnIt->get<std::vector<BPFunction>>();
        }
    } catch (const std::exception& e) {
        throw BlueprintReaderError(
            fmt::format("malformed fixture {}: {}", file.string(), e.what()));
    }

    if (entry.summary.AssetPath.empty()) {
        throw BlueprintReaderError(
            fmt::format("fixture {} has empty summary.asset_path", file.string()));
    }

    auto key = entry.summary.AssetPath;
    if (assets_.find(key) != assets_.end()) {
        throw BlueprintReaderError(
            fmt::format("duplicate fixture asset_path: {}", key));
    }
    assets_.emplace(std::move(key), std::move(entry));
}

const MockBlueprintReader::FixtureEntry&
MockBlueprintReader::Require(std::string_view assetPath) const {
    auto it = assets_.find(assetPath);
    if (it == assets_.end()) {
        throw AssetNotFound(fmt::format("asset not found: {}", assetPath));
    }
    return it->second;
}

std::vector<BPAssetSummary> MockBlueprintReader::ListBlueprints(std::string_view path) {
    std::vector<BPAssetSummary> out;
    for (const auto& [k, entry] : assets_) {
        if (PathMatches(path, entry.summary.AssetPath)) {
            out.push_back(entry.summary);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const BPAssetSummary& a, const BPAssetSummary& b) {
                  return a.AssetPath < b.AssetPath;
              });
    return out;
}

BPMetadata MockBlueprintReader::ReadBlueprint(std::string_view assetPath) {
    return Require(assetPath).metadata;
}

BPGraph MockBlueprintReader::GetGraph(std::string_view assetPath,
                                      std::string_view graphName) {
    const auto& entry = Require(assetPath);
    for (const auto& g : entry.graphs) {
        if (g.Name == graphName) return g;
    }
    throw BlueprintReaderError(
        fmt::format("graph not found in {}: {}", assetPath, graphName));
}

BPFunction MockBlueprintReader::GetFunction(std::string_view assetPath,
                                            std::string_view fnName) {
    const auto& entry = Require(assetPath);
    for (const auto& f : entry.functions) {
        if (f.Name == fnName) return f;
    }
    throw BlueprintReaderError(
        fmt::format("function not found in {}: {}", assetPath, fnName));
}

std::vector<BPVariable> MockBlueprintReader::ListVariables(std::string_view assetPath) {
    return Require(assetPath).metadata.Variables;
}

void MockBlueprintReader::AddVariable(std::string_view, std::string_view,
                                      const BPPinType&, std::string_view,
                                      std::string_view, bool, bool) {
    throw BlueprintReaderError(
        "AddVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetNodePosition(std::string_view, std::string_view,
                                          std::string_view, int, int) {
    throw BlueprintReaderError(
        "SetNodePosition: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteNode(std::string_view, std::string_view,
                                     std::string_view) {
    throw BlueprintReaderError(
        "DeleteNode: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

std::string MockBlueprintReader::AddNode(std::string_view, std::string_view,
                                         std::string_view, int, int,
                                         const std::map<std::string, std::string, std::less<>>&) {
    throw BlueprintReaderError(
        "AddNode: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::WirePins(std::string_view, std::string_view,
                                   std::string_view, std::string_view,
                                   std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "WirePins: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteVariable(std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "DeleteVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::RenameVariable(std::string_view, std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "RenameVariable: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

std::string MockBlueprintReader::AddFunction(std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "AddFunction: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::AddFunctionInput(std::string_view, std::string_view,
                                           std::string_view, const BPPinType&) {
    throw BlueprintReaderError(
        "AddFunctionInput: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::AddFunctionOutput(std::string_view, std::string_view,
                                            std::string_view, const BPPinType&) {
    throw BlueprintReaderError(
        "AddFunctionOutput: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::DeleteFunction(std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "DeleteFunction: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

void MockBlueprintReader::SetVariableDefault(std::string_view, std::string_view, std::string_view) {
    throw BlueprintReaderError(
        "SetVariableDefault: mock backend is read-only; set BP_READER_BACKEND=commandlet");
}

std::vector<BPNode> MockBlueprintReader::FindNode(std::string_view assetPath,
                                                  std::string_view query,
                                                  std::string_view kind) {
    const auto& entry = Require(assetPath);
    std::vector<BPNode> out;
    const std::string kindLower = kind.empty() ? std::string{} : LowerAscii(kind);
    auto matchKind = [&](const BPNode& n) -> bool {
        if (kindLower.empty()) return true;
        if (!n.Meta.is_object()) return false;
        auto it = n.Meta.find("kind");
        if (it == n.Meta.end() || !it->is_string()) return false;
        return LowerAscii(it->get<std::string>()) == kindLower;
    };
    auto match = [&](const BPNode& n) {
        if (!matchKind(n)) return false;
        if (query.empty()) return true;
        return ContainsCI(n.Class, query) || ContainsCI(n.Title, query);
    };
    for (const auto& g : entry.graphs) {
        for (const auto& n : g.Nodes) {
            if (match(n)) out.push_back(n);
        }
    }
    for (const auto& f : entry.functions) {
        for (const auto& n : f.Graph.Nodes) {
            if (match(n)) out.push_back(n);
        }
    }
    return out;
}

} // namespace bpr::backends
