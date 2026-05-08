// CachingBlueprintReader — decorator around IBlueprintReader that
// memoizes read responses for a configurable TTL and invalidates on
// writes.
//
// Why: AI clients do flurries of reads in a session. A typical "tell me
// about BP_Enemy" question routes through ReadBlueprint -> ListVariables
// -> GetGraph in succession, and the agent often retries with different
// projections. Round-tripping each call to the editor commandlet costs
// 50–500 ms; all of that is duplicate work for the same .uasset.
//
// Cache semantics:
//   - Each read call is keyed by (operation, asset_path, *extras*).
//   - Entries expire after `ttl` (default 30 s).
//   - Any write tool invalidates ALL cached entries for the affected
//     asset_path. ListBlueprints is invalidated by any write because the
//     `modified_iso` summary changes.
//   - Cache is in-memory, per-process, NOT shared across server runs.
//
// Trade-off: TTL is the simplest correct invalidation strategy that
// doesn't require us to know .uasset on-disk paths. For longer cache
// lifetimes you'd want mtime-based invalidation; until users complain
// about staleness, TTL is plenty.
//
// Thread-safety: a single mutex guards the cache. The MCP server
// processes one request at a time, so contention is effectively zero —
// the mutex exists so the decorator stays safe if we ever multiplex.

#pragma once

#include "backends/IBlueprintReader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace bpr::backends {

class CachingBlueprintReader : public IBlueprintReader {
public:
    struct Stats {
        std::atomic<std::uint64_t> hits{0};
        std::atomic<std::uint64_t> misses{0};
        std::atomic<std::uint64_t> invalidations{0};
    };

    // TTL is stored in milliseconds internally so tests can use sub-second
    // values without losing precision. Production callers pass `seconds`
    // (from env vars) and rely on the implicit duration conversion.
    //
    // C2: when `projectDir` is non-empty, the cache also stamps each entry
    // with the .uasset file's mtime at insert time and re-checks on lookup.
    // External editor edits to the same asset invalidate the entry even if
    // its TTL hasn't expired. Empty projectDir disables mtime checking.
    CachingBlueprintReader(std::unique_ptr<IBlueprintReader> inner,
                           std::chrono::milliseconds ttl,
                           std::filesystem::path projectDir = {});

    // ----- read tools (cached) -----------------------------------------
    std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override;
    BPMetadata                  ReadBlueprint(std::string_view assetPath) override;
    BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) override;
    BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) override;
    std::vector<BPVariable>     ListVariables(std::string_view assetPath) override;
    std::vector<BPComponent>    GetComponents(std::string_view assetPath) override;
    std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                          std::string_view kind = {}) override;

    // ----- write tools (pass-through + invalidate) ---------------------
    void AddVariable(std::string_view assetPath, std::string_view name,
                     const BPPinType& type, std::string_view defaultValue,
                     std::string_view category, bool replicated, bool editable) override;
    void SetNodePosition(std::string_view assetPath, std::string_view graphName,
                         std::string_view nodeId, int x, int y) override;
    void DeleteNode(std::string_view assetPath, std::string_view graphName,
                    std::string_view nodeId) override;
    std::string AddNode(std::string_view assetPath, std::string_view graphName,
                        std::string_view kind, int x, int y,
                        const std::map<std::string, std::string, std::less<>>& extras) override;
    void WirePins(std::string_view assetPath, std::string_view graphName,
                  std::string_view fromNodeId, std::string_view fromPinSpec,
                  std::string_view toNodeId,   std::string_view toPinSpec) override;
    void DeleteVariable(std::string_view assetPath, std::string_view name) override;
    void RenameVariable(std::string_view assetPath, std::string_view oldName,
                        std::string_view newName) override;
    AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) override;
    void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                          std::string_view paramName, const BPPinType& type) override;
    void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                           std::string_view paramName, const BPPinType& type) override;
    void DeleteFunction(std::string_view assetPath, std::string_view name) override;
    void SetVariableDefault(std::string_view assetPath, std::string_view name,
                            std::string_view newDefault) override;
    CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
                                          std::string_view parentClass) override;
    void SetPinDefault(std::string_view assetPath, std::string_view graphName,
                       std::string_view nodeId, std::string_view pinSpec,
                       std::string_view value) override;
    void RetypeVariable(std::string_view assetPath, std::string_view name,
                        const BPPinType& newType) override;
    void SetVariableCategory(std::string_view assetPath, std::string_view name,
                             std::string_view category) override;
    DuplicateBlueprintResult DuplicateBlueprint(std::string_view sourceAssetPath,
                                                std::string_view destAssetPath) override;

    // Batch sentinels (A1) — forwards to inner and tracks depth so
    // invalidations triggered by writes during a batch don't drop entries
    // that subsequent ops in the same batch would re-fetch. Flushed by
    // EndBatch's trailing call to InvalidateAsset for each pending entry.
    void BeginBatch() override;
    nlohmann::json EndBatch(bool skipCompile = false) override;
    nlohmann::json ShutdownDaemon() override;

    // Drop everything for `assetPath`, plus the global ListBlueprints
    // cache. Public so callers / tests can force-clear.
    void InvalidateAsset(std::string_view assetPath);
    void InvalidateAll();

    const Stats& GetStats() const { return stats_; }

private:
    struct Entry {
        std::chrono::steady_clock::time_point inserted;
        // Each key uniquely identifies the operation+args, so the void*
        // payload always has the same concrete type per key. We use
        // shared_ptr<const T> at call sites and static_pointer_cast back
        // out — type-safety is enforced by *call site discipline*, not
        // by the cache itself.
        std::shared_ptr<const void> value;
        // C2: mtime stamp captured at insert. Zero-time means "no source
        // file resolved" (e.g. ListBlueprints across a directory) — those
        // entries skip mtime checks.
        std::filesystem::file_time_type sourceMtime{};
        bool hasMtime = false;
    };

    // Look up (or compute) the entry for `key`. Pass the asset path so
    // the reverse index can be maintained for fast invalidation.
    // `compute` runs OUTSIDE the mutex to keep the editor commandlet's
    // round-trip off the critical section.
    std::shared_ptr<const void> LookupOrCompute(
        const std::string& key, std::string_view assetPath,
        const std::function<std::shared_ptr<const void>()>& compute);

    std::unique_ptr<IBlueprintReader> inner_;
    std::chrono::milliseconds ttl_;
    // C2: project root for resolving /Game/X → <root>/Content/X.uasset.
    // Empty disables mtime checks.
    std::filesystem::path projectDir_;

    mutable std::mutex mu_;
    std::map<std::string, Entry> entries_;
    // assetPath -> set of keys that should be dropped when the asset is
    // mutated. The empty key "" gathers global keys (e.g. ListBlueprints).
    std::map<std::string, std::set<std::string>> byAsset_;

    // Batch state (A1): depth counter so nested batches behave; pending
    // asset invalidations recorded during the batch and flushed at the
    // outermost EndBatch.
    int batchDepth_ = 0;
    std::set<std::string> pendingInvalidations_;
    bool pendingGlobalInvalidation_ = false;

    Stats stats_;
};

// Convenience factory: matches the BackendFactory style. Takes ownership
// of `inner`. If ttl <= 0 returns `inner` unwrapped (caching disabled).
// `projectDir` enables mtime-based cache invalidation (C2) when set.
std::unique_ptr<IBlueprintReader> WrapWithCache(
    std::unique_ptr<IBlueprintReader> inner, std::chrono::seconds ttl,
    std::filesystem::path projectDir = {});

} // namespace bpr::backends
