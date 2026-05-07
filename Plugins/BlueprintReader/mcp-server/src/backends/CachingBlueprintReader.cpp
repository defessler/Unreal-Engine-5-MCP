#include "backends/CachingBlueprintReader.h"

#include <fmt/core.h>

namespace bpr::backends {

namespace {

// Build a stable, collision-free cache key from operation + args.
// Format: "<op>|<asset>|<extra1>|<extra2>". The pipe is fine — UE asset
// paths can contain '/', but never '|'. An empty asset is allowed (e.g.
// ListBlueprints uses path scope, not asset).
std::string MakeKey(std::string_view op, std::string_view asset,
                    std::string_view extra1 = {}, std::string_view extra2 = {}) {
    std::string out;
    out.reserve(op.size() + asset.size() + extra1.size() + extra2.size() + 4);
    out.append(op).push_back('|');
    out.append(asset).push_back('|');
    out.append(extra1).push_back('|');
    out.append(extra2);
    return out;
}

} // namespace

CachingBlueprintReader::CachingBlueprintReader(
    std::unique_ptr<IBlueprintReader> inner,
    std::chrono::milliseconds ttl)
    : inner_(std::move(inner)), ttl_(ttl) {}

std::shared_ptr<const void> CachingBlueprintReader::LookupOrCompute(
    const std::string& key, std::string_view assetPath,
    const std::function<std::shared_ptr<const void>()>& compute) {

    {
        std::lock_guard lock(mu_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            auto age = std::chrono::steady_clock::now() - it->second.inserted;
            if (age < ttl_) {
                stats_.hits.fetch_add(1, std::memory_order_relaxed);
                return it->second.value;
            }
            // Expired — fall through to recompute. We could erase here,
            // but the upcoming insert will overwrite it.
        }
    }

    stats_.misses.fetch_add(1, std::memory_order_relaxed);

    // Compute outside the mutex so commandlet round-trips don't serialize
    // unrelated requests.
    auto value = compute();

    {
        std::lock_guard lock(mu_);
        entries_[key] = Entry{std::chrono::steady_clock::now(), value};
        byAsset_[std::string(assetPath)].insert(key);
    }
    return value;
}

void CachingBlueprintReader::InvalidateAsset(std::string_view assetPath) {
    std::lock_guard lock(mu_);
    auto it = byAsset_.find(std::string(assetPath));
    if (it != byAsset_.end()) {
        for (const auto& k : it->second) entries_.erase(k);
        byAsset_.erase(it);
    }
    // ListBlueprints is keyed under "" — drop those too because any
    // asset write changes the modified_iso summary.
    auto globalIt = byAsset_.find("");
    if (globalIt != byAsset_.end()) {
        for (const auto& k : globalIt->second) entries_.erase(k);
        byAsset_.erase(globalIt);
    }
    stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
}

void CachingBlueprintReader::InvalidateAll() {
    std::lock_guard lock(mu_);
    entries_.clear();
    byAsset_.clear();
    stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// Read tools — cached
// ============================================================================

std::vector<BPAssetSummary>
CachingBlueprintReader::ListBlueprints(std::string_view path) {
    auto key = MakeKey("list", "", path);
    auto sp = LookupOrCompute(key, "", [&] {
        auto v = std::make_shared<const std::vector<BPAssetSummary>>(
            inner_->ListBlueprints(path));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const std::vector<BPAssetSummary>>(sp);
}

BPMetadata CachingBlueprintReader::ReadBlueprint(std::string_view assetPath) {
    auto key = MakeKey("read", assetPath);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const BPMetadata>(inner_->ReadBlueprint(assetPath));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const BPMetadata>(sp);
}

BPGraph CachingBlueprintReader::GetGraph(std::string_view assetPath,
                                         std::string_view graphName) {
    auto key = MakeKey("graph", assetPath, graphName);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const BPGraph>(inner_->GetGraph(assetPath, graphName));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const BPGraph>(sp);
}

BPFunction CachingBlueprintReader::GetFunction(std::string_view assetPath,
                                               std::string_view fnName) {
    auto key = MakeKey("fn", assetPath, fnName);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const BPFunction>(inner_->GetFunction(assetPath, fnName));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const BPFunction>(sp);
}

std::vector<BPVariable>
CachingBlueprintReader::ListVariables(std::string_view assetPath) {
    auto key = MakeKey("vars", assetPath);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const std::vector<BPVariable>>(
            inner_->ListVariables(assetPath));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const std::vector<BPVariable>>(sp);
}

std::vector<BPComponent>
CachingBlueprintReader::GetComponents(std::string_view assetPath) {
    auto key = MakeKey("components", assetPath);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const std::vector<BPComponent>>(
            inner_->GetComponents(assetPath));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const std::vector<BPComponent>>(sp);
}

std::vector<BPNode>
CachingBlueprintReader::FindNode(std::string_view assetPath,
                                 std::string_view query,
                                 std::string_view kind) {
    auto key = MakeKey("findnode", assetPath, query, kind);
    auto sp = LookupOrCompute(key, assetPath, [&] {
        auto v = std::make_shared<const std::vector<BPNode>>(
            inner_->FindNode(assetPath, query, kind));
        return std::static_pointer_cast<const void>(v);
    });
    return *std::static_pointer_cast<const std::vector<BPNode>>(sp);
}

// ============================================================================
// Write tools — pass-through, then drop cache for the asset
// ============================================================================

void CachingBlueprintReader::AddVariable(std::string_view assetPath, std::string_view name,
                                         const BPPinType& type, std::string_view defaultValue,
                                         std::string_view category, bool replicated, bool editable) {
    inner_->AddVariable(assetPath, name, type, defaultValue, category, replicated, editable);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::SetNodePosition(std::string_view assetPath, std::string_view graphName,
                                             std::string_view nodeId, int x, int y) {
    inner_->SetNodePosition(assetPath, graphName, nodeId, x, y);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::DeleteNode(std::string_view assetPath, std::string_view graphName,
                                        std::string_view nodeId) {
    inner_->DeleteNode(assetPath, graphName, nodeId);
    InvalidateAsset(assetPath);
}

std::string CachingBlueprintReader::AddNode(std::string_view assetPath, std::string_view graphName,
                                            std::string_view kind, int x, int y,
                                            const std::map<std::string, std::string, std::less<>>& extras) {
    auto id = inner_->AddNode(assetPath, graphName, kind, x, y, extras);
    InvalidateAsset(assetPath);
    return id;
}

void CachingBlueprintReader::WirePins(std::string_view assetPath, std::string_view graphName,
                                      std::string_view fromNodeId, std::string_view fromPinSpec,
                                      std::string_view toNodeId, std::string_view toPinSpec) {
    inner_->WirePins(assetPath, graphName, fromNodeId, fromPinSpec, toNodeId, toPinSpec);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::DeleteVariable(std::string_view assetPath, std::string_view name) {
    inner_->DeleteVariable(assetPath, name);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::RenameVariable(std::string_view assetPath, std::string_view oldName,
                                            std::string_view newName) {
    inner_->RenameVariable(assetPath, oldName, newName);
    InvalidateAsset(assetPath);
}

std::string CachingBlueprintReader::AddFunction(std::string_view assetPath, std::string_view name) {
    auto out = inner_->AddFunction(assetPath, name);
    InvalidateAsset(assetPath);
    return out;
}

void CachingBlueprintReader::AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                                              std::string_view paramName, const BPPinType& type) {
    inner_->AddFunctionInput(assetPath, functionName, paramName, type);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                                               std::string_view paramName, const BPPinType& type) {
    inner_->AddFunctionOutput(assetPath, functionName, paramName, type);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::DeleteFunction(std::string_view assetPath, std::string_view name) {
    inner_->DeleteFunction(assetPath, name);
    InvalidateAsset(assetPath);
}

void CachingBlueprintReader::SetVariableDefault(std::string_view assetPath, std::string_view name,
                                                std::string_view newDefault) {
    inner_->SetVariableDefault(assetPath, name, newDefault);
    InvalidateAsset(assetPath);
}

// ============================================================================
// Factory helper
// ============================================================================

std::unique_ptr<IBlueprintReader> WrapWithCache(
    std::unique_ptr<IBlueprintReader> inner, std::chrono::seconds ttl) {
    if (ttl <= std::chrono::seconds(0)) return inner;
    return std::make_unique<CachingBlueprintReader>(std::move(inner), ttl);
}

} // namespace bpr::backends
