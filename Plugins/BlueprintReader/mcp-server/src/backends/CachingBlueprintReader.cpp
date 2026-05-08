#include "backends/CachingBlueprintReader.h"

#include <fmt/core.h>

#include <optional>
#include <system_error>

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
    std::chrono::milliseconds ttl,
    std::filesystem::path projectDir)
    : inner_(std::move(inner)), ttl_(ttl), projectDir_(std::move(projectDir)) {}

namespace {
// Resolve a UE asset path under /Game/ to its on-disk .uasset file
// inside `<project>/Content/`. Returns empty path if the asset is outside
// /Game/ (plugin content, /Engine/...) or if projectDir is empty.
std::filesystem::path ResolveUasset(const std::filesystem::path& projectDir,
                                    std::string_view assetPath) {
    if (projectDir.empty() || assetPath.empty()) return {};
    constexpr std::string_view kGame = "/Game/";
    if (assetPath.size() < kGame.size() ||
        assetPath.compare(0, kGame.size(), kGame) != 0) {
        return {};
    }
    std::string_view rest = assetPath.substr(kGame.size());
    // projectDir may be a .uproject path or its containing dir. Normalize:
    // if it's a file, take parent.
    std::filesystem::path root = projectDir;
    if (root.has_extension()) root = root.parent_path();
    return root / "Content" / (std::string(rest) + ".uasset");
}

// Stat the file's mtime; returns nullopt on any failure (file missing,
// permission, etc.) — caller treats that as "no mtime info, skip check".
std::optional<std::filesystem::file_time_type>
SafeMtime(const std::filesystem::path& p) {
    if (p.empty()) return std::nullopt;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return std::nullopt;
    return t;
}
} // namespace

std::shared_ptr<const void> CachingBlueprintReader::LookupOrCompute(
    const std::string& key, std::string_view assetPath,
    const std::function<std::shared_ptr<const void>()>& compute) {

    // C2: capture the source file's mtime before going into the lock so
    // a slow filesystem doesn't block other lookups. The same mtime is
    // re-used on insert below.
    auto sourcePath = ResolveUasset(projectDir_, assetPath);
    auto currentMtime = SafeMtime(sourcePath);

    {
        std::lock_guard lock(mu_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            auto age = std::chrono::steady_clock::now() - it->second.inserted;
            // C2: mtime mismatch evicts even if TTL hasn't expired.
            bool mtimeStale = it->second.hasMtime && currentMtime &&
                              it->second.sourceMtime != *currentMtime;
            if (age < ttl_ && !mtimeStale) {
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

    // After compute(), re-stat — the file may have been written by the
    // compute itself (e.g. a write op flushed the BP). Using the post-
    // compute mtime ensures the entry isn't immediately stale.
    auto postMtime = SafeMtime(sourcePath);
    if (!postMtime) postMtime = currentMtime;  // fall back to the pre-stat

    {
        std::lock_guard lock(mu_);
        Entry e;
        e.inserted = std::chrono::steady_clock::now();
        e.value    = value;
        if (postMtime) {
            e.sourceMtime = *postMtime;
            e.hasMtime    = true;
        }
        entries_[key] = std::move(e);
        byAsset_[std::string(assetPath)].insert(key);
    }
    return value;
}

void CachingBlueprintReader::InvalidateAsset(std::string_view assetPath) {
    std::lock_guard lock(mu_);
    // During a batch, defer the actual eviction until EndBatch — otherwise
    // a write op early in the batch would drop entries that subsequent ops
    // in the same batch are about to re-fetch (defeating the cache).
    if (batchDepth_ > 0) {
        pendingInvalidations_.insert(std::string(assetPath));
        pendingGlobalInvalidation_ = true;  // ListBlueprints summaries change
        return;
    }
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
    pendingInvalidations_.clear();
    pendingGlobalInvalidation_ = false;
    stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
}

nlohmann::json CachingBlueprintReader::ShutdownDaemon() {
    // Drop the cache too — any cached entries point at a now-stale
    // in-memory state that will be re-fetched fresh on next read.
    InvalidateAll();
    return inner_->ShutdownDaemon();
}

// ----- Batch sentinels (A1) ------------------------------------------------
void CachingBlueprintReader::BeginBatch() {
    {
        std::lock_guard lock(mu_);
        ++batchDepth_;
    }
    inner_->BeginBatch();
}

nlohmann::json CachingBlueprintReader::EndBatch(bool skipCompile) {
    nlohmann::json flushAck = inner_->EndBatch(skipCompile);
    std::set<std::string> toInvalidate;
    bool flushGlobal = false;
    {
        std::lock_guard lock(mu_);
        if (batchDepth_ > 0) --batchDepth_;
        if (batchDepth_ == 0) {
            toInvalidate = std::move(pendingInvalidations_);
            pendingInvalidations_.clear();
            flushGlobal = pendingGlobalInvalidation_;
            pendingGlobalInvalidation_ = false;
        }
    }
    // Flush outside the lock — InvalidateAsset reacquires it. The deferred
    // flag is now clear, so these will run their normal eviction path.
    for (const auto& asset : toInvalidate) {
        InvalidateAsset(asset);
    }
    if (flushGlobal && toInvalidate.empty()) {
        // Edge case: writes happened with no asset key (shouldn't normally
        // occur, but make ListBlueprints invalidation correct anyway).
        std::lock_guard lock(mu_);
        auto globalIt = byAsset_.find("");
        if (globalIt != byAsset_.end()) {
            for (const auto& k : globalIt->second) entries_.erase(k);
            byAsset_.erase(globalIt);
            stats_.invalidations.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return flushAck;
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

IBlueprintReader::AddFunctionResult
CachingBlueprintReader::AddFunction(std::string_view assetPath, std::string_view name) {
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

IBlueprintReader::CreateBlueprintResult
CachingBlueprintReader::CreateBlueprint(std::string_view assetPath,
                                        std::string_view parentClass) {
    auto out = inner_->CreateBlueprint(assetPath, parentClass);
    // New asset → drop ListBlueprints cache and any stale entries for this path.
    InvalidateAsset(assetPath);
    return out;
}

void CachingBlueprintReader::SetPinDefault(std::string_view assetPath,
                                           std::string_view graphName,
                                           std::string_view nodeId,
                                           std::string_view pinSpec,
                                           std::string_view value) {
    inner_->SetPinDefault(assetPath, graphName, nodeId, pinSpec, value);
    InvalidateAsset(assetPath);
}

// ============================================================================
// Factory helper
// ============================================================================

std::unique_ptr<IBlueprintReader> WrapWithCache(
    std::unique_ptr<IBlueprintReader> inner, std::chrono::seconds ttl,
    std::filesystem::path projectDir) {
    if (ttl <= std::chrono::seconds(0)) return inner;
    return std::make_unique<CachingBlueprintReader>(
        std::move(inner), ttl, std::move(projectDir));
}

} // namespace bpr::backends
