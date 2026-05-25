#include "backends/CachingBlueprintReader.h"

#include <optional>
#include <system_error>

namespace bpr::backends {

namespace caching_blueprint_reader_detail {

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

}    // namespace caching_blueprint_reader_detail
using namespace caching_blueprint_reader_detail;

CachingBlueprintReader::CachingBlueprintReader(
	std::unique_ptr<IBlueprintReader> inner,
	std::chrono::milliseconds ttl,
	std::filesystem::path projectDir)
	: inner_(std::move(inner)), ttl_(ttl), projectDir_(std::move(projectDir)) {}

namespace caching_blueprint_reader_detail2 {
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
	if (root.has_extension())
	{
		root = root.parent_path();
	}
	return root / "Content" / (std::string(rest) + ".uasset");
}

// Stat the file's mtime; returns nullopt on any failure (file missing,
// permission, etc.) — caller treats that as "no mtime info, skip check".
std::optional<std::filesystem::file_time_type>
SafeMtime(const std::filesystem::path& p) {
	if (p.empty())
	{
		return std::nullopt;
	}
	std::error_code ec;
	auto t = std::filesystem::last_write_time(p, ec);
	if (ec)
	{
		return std::nullopt;
	}
	return t;
}
}    // namespace caching_blueprint_reader_detail2
using namespace caching_blueprint_reader_detail2;

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
	if (!postMtime)
	{
		postMtime = currentMtime;  // fall back to the pre-stat
	}

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
		for (const auto& k : it->second)
		{
			entries_.erase(k);
		}
		byAsset_.erase(it);
	}
	// ListBlueprints is keyed under "" — drop those too because any
	// asset write changes the modified_iso summary.
	auto globalIt = byAsset_.find("");
	if (globalIt != byAsset_.end()) {
		for (const auto& k : globalIt->second)
		{
			entries_.erase(k);
		}
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
		if (batchDepth_ > 0)
		{
			--batchDepth_;
		}
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
			for (const auto& k : globalIt->second)
			{
				entries_.erase(k);
			}
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

// Exec tool: forward to inner. Arbitrary Python can mutate any asset, but
// we can't know which — rely on the short read-cache TTL to expire stale
// entries rather than blanket-clearing on every call.
IBlueprintReader::PythonResult CachingBlueprintReader::RunPythonScript(std::string_view code) {
	return inner_->RunPythonScript(code);
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

void CachingBlueprintReader::RetypeVariable(std::string_view assetPath,
											std::string_view name,
											const BPPinType& newType) {
	inner_->RetypeVariable(assetPath, name, newType);
	InvalidateAsset(assetPath);
}

void CachingBlueprintReader::SetVariableCategory(std::string_view assetPath,
												 std::string_view name,
												 std::string_view category) {
	inner_->SetVariableCategory(assetPath, name, category);
	InvalidateAsset(assetPath);
}

IBlueprintReader::DuplicateBlueprintResult
CachingBlueprintReader::DuplicateBlueprint(std::string_view sourceAssetPath,
										   std::string_view destAssetPath) {
	auto out = inner_->DuplicateBlueprint(sourceAssetPath, destAssetPath);
	// New asset → drop the destination's cache (in case anything was
	// stale from a prior delete) and the global ListBlueprints cache.
	InvalidateAsset(destAssetPath);
	return out;
}

IBlueprintReader::WriteGeneratedSourceResult
CachingBlueprintReader::WriteGeneratedSource(std::string_view destPath,
											 std::string_view content,
											 bool createDirs) {
	// No cache invalidation needed — generated source files don't
	// affect BP asset registry / .uasset state.
	return inner_->WriteGeneratedSource(destPath, content, createDirs);
}

nlohmann::json CachingBlueprintReader::StructuralDiff(
	std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
	return inner_->StructuralDiff(a, b, opts);
}

// ----- Asset-registry queries (pass-through) -----------------------------

IBlueprintReader::AssetRegistryListResult
CachingBlueprintReader::ListAssets(std::string_view path, bool recursive) {
	return inner_->ListAssets(path, recursive);
}

IBlueprintReader::AssetRegistryListResult
CachingBlueprintReader::FindAsset(std::string_view query, std::string_view path) {
	return inner_->FindAsset(query, path);
}

// ----- Project + Content Browser ops (pass-through with invalidation) ----

IBlueprintReader::ProjectMetadata
CachingBlueprintReader::GetProjectMetadata() {
	return inner_->GetProjectMetadata();
}

IBlueprintReader::SaveAllResult
CachingBlueprintReader::SaveAll(bool dirtyOnly) {
	// SaveAll doesn't change in-memory asset state, just persists it —
	// no invalidation needed.
	return inner_->SaveAll(dirtyOnly);
}

IBlueprintReader::MoveAssetResult
CachingBlueprintReader::MoveAsset(std::string_view sourcePath,
								  std::string_view destPath) {
	auto out = inner_->MoveAsset(sourcePath, destPath);
	// Both ends of the move can affect cached entries; drop the global
	// list cache and any per-asset entries on either side.
	InvalidateAsset(sourcePath);
	InvalidateAsset(destPath);
	return out;
}

IBlueprintReader::DeleteAssetResult
CachingBlueprintReader::DeleteAsset(std::string_view assetPath, bool force) {
	auto out = inner_->DeleteAsset(assetPath, force);
	if (out.deleted)
	{
		InvalidateAsset(assetPath);
	}
	return out;
}

IBlueprintReader::CreateFolderResult
CachingBlueprintReader::CreateFolder(std::string_view folderPath) {
	auto out = inner_->CreateFolder(folderPath);
	// List cache may now show the new folder; drop the global key.
	InvalidateAsset("");
	return out;
}

std::vector<BPAssetSummary>
CachingBlueprintReader::ListDataTables(std::string_view path) {
	// Pass through (no caching for this minor surface; can add later).
	return inner_->ListDataTables(path);
}

IBlueprintReader::DataTableInfo
CachingBlueprintReader::ReadDataTable(std::string_view assetPath) {
	return inner_->ReadDataTable(assetPath);
}

IBlueprintReader::AddDataRowResult
CachingBlueprintReader::AddDataRow(std::string_view a, std::string_view n,
								   const nlohmann::json& v, bool o) {
	auto out = inner_->AddDataRow(a, n, v, o);
	InvalidateAsset(a);  // row count + table contents changed
	return out;
}

IBlueprintReader::SetDataRowValueResult
CachingBlueprintReader::SetDataRowValue(std::string_view a, std::string_view r,
										std::string_view f, std::string_view v) {
	auto out = inner_->SetDataRowValue(a, r, f, v);
	InvalidateAsset(a);
	return out;
}

IBlueprintReader::AddComponentResult
CachingBlueprintReader::AddComponent(std::string_view a, std::string_view n,
									 std::string_view c, std::string_view p,
									 std::string_view s) {
	auto out = inner_->AddComponent(a, n, c, p, s);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::RemoveComponentResult
CachingBlueprintReader::RemoveComponent(std::string_view a, std::string_view n) {
	auto out = inner_->RemoveComponent(a, n);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::AttachComponentResult
CachingBlueprintReader::AttachComponent(std::string_view a, std::string_view n,
										std::string_view p, std::string_view s) {
	auto out = inner_->AttachComponent(a, n, p, s);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetComponentPropertyResult
CachingBlueprintReader::SetComponentProperty(std::string_view a, std::string_view c,
											 std::string_view p, std::string_view v) {
	auto out = inner_->SetComponentProperty(a, c, p, v);
	InvalidateAsset(a);
	return out;
}

// ----- Live editor ops (pass-through) ------------------------------------

IBlueprintReader::ConsoleCommandResult
CachingBlueprintReader::ConsoleCommand(std::string_view c) { return inner_->ConsoleCommand(c); }
IBlueprintReader::CVarValue
CachingBlueprintReader::GetCVar(std::string_view n) { return inner_->GetCVar(n); }
IBlueprintReader::CVarValue
CachingBlueprintReader::SetCVar(std::string_view n, std::string_view v) {
	return inner_->SetCVar(n, v);
}
IBlueprintReader::PieResult
CachingBlueprintReader::PieStart(std::string_view m) { return inner_->PieStart(m); }
IBlueprintReader::PieResult
CachingBlueprintReader::PieStop() { return inner_->PieStop(); }
IBlueprintReader::LiveCodingResult
CachingBlueprintReader::LiveCodingCompile() { return inner_->LiveCodingCompile(); }
IBlueprintReader::SelectionResult
CachingBlueprintReader::GetSelectedActors() { return inner_->GetSelectedActors(); }
IBlueprintReader::SelectionResult
CachingBlueprintReader::SetSelection(const std::vector<std::string>& names, bool replace) {
	return inner_->SetSelection(names, replace);
}
IBlueprintReader::SpawnActorResult
CachingBlueprintReader::SpawnActor(std::string_view cp,
	double lx, double ly, double lz,
	double rp, double ry, double rr,
	double sx, double sy, double sz) {
	return inner_->SpawnActor(cp, lx, ly, lz, rp, ry, rr, sx, sy, sz);
}
void CachingBlueprintReader::SetActorTransform(std::string_view n,
	double lx, double ly, double lz,
	double rp, double ry, double rr,
	double sx, double sy, double sz) {
	inner_->SetActorTransform(n, lx, ly, lz, rp, ry, rr, sx, sy, sz);
}
IBlueprintReader::DeleteActorResult
CachingBlueprintReader::DeleteActor(std::string_view n) { return inner_->DeleteActor(n); }
IBlueprintReader::OutputLogResult
CachingBlueprintReader::ReadOutputLog(int limit, std::string_view minSev) {
	return inner_->ReadOutputLog(limit, minSev);
}
IBlueprintReader::AutomationRunResult
CachingBlueprintReader::RunAutomationTests(std::string_view pattern) {
	return inner_->RunAutomationTests(pattern);
}

// ----- Phase 8 EA-pull Wave 1 (uncacheable live state) ------------------

IBlueprintReader::OpenAssetsResult CachingBlueprintReader::ListOpenAssets() {
	return inner_->ListOpenAssets();
}
IBlueprintReader::ActiveAssetResult CachingBlueprintReader::GetActiveAsset() {
	return inner_->GetActiveAsset();
}
IBlueprintReader::CompileStatusResult
CachingBlueprintReader::GetCompileStatus(std::string_view a) {
	return inner_->GetCompileStatus(a);
}
IBlueprintReader::DirtyPackagesResult CachingBlueprintReader::GetDirtyPackages() {
	return inner_->GetDirtyPackages();
}
IBlueprintReader::FocusedWindowResult CachingBlueprintReader::GetFocusedWindow() {
	return inner_->GetFocusedWindow();
}
IBlueprintReader::PieStateResult CachingBlueprintReader::GetPieState() {
	return inner_->GetPieState();
}
IBlueprintReader::ModalStateResult CachingBlueprintReader::GetModalState() {
	return inner_->GetModalState();
}
IBlueprintReader::EditorModesResult CachingBlueprintReader::GetActiveEditorMode() {
	return inner_->GetActiveEditorMode();
}
IBlueprintReader::FocusedWidgetResult CachingBlueprintReader::GetFocusedWidget() {
	return inner_->GetFocusedWidget();
}
IBlueprintReader::OpenAssetEditorResult
CachingBlueprintReader::OpenAssetEditor(std::string_view a) {
	return inner_->OpenAssetEditor(a);
}
IBlueprintReader::CloseAssetEditorResult
CachingBlueprintReader::CloseAssetEditor(std::string_view a) {
	return inner_->CloseAssetEditor(a);
}
IBlueprintReader::CameraTransformResult CachingBlueprintReader::GetCameraTransform() {
	return inner_->GetCameraTransform();
}
IBlueprintReader::ViewModeResult CachingBlueprintReader::GetViewMode() {
	return inner_->GetViewMode();
}
IBlueprintReader::ShowFlagsResult CachingBlueprintReader::GetShowFlags() {
	return inner_->GetShowFlags();
}
IBlueprintReader::SelectedComponentsResult CachingBlueprintReader::GetSelectedComponents() {
	return inner_->GetSelectedComponents();
}
IBlueprintReader::ContentBrowserSelectionResult CachingBlueprintReader::GetSelectedAssets() {
	return inner_->GetSelectedAssets();
}
IBlueprintReader::ContentBrowserSelectionResult
CachingBlueprintReader::SetSelectedAssets(const std::vector<std::string>& a) {
	return inner_->SetSelectedAssets(a);
}
IBlueprintReader::ContentBrowserFoldersResult CachingBlueprintReader::GetSelectedFolders() {
	return inner_->GetSelectedFolders();
}
IBlueprintReader::ContentBrowserPathResult CachingBlueprintReader::GetContentBrowserPath() {
	return inner_->GetContentBrowserPath();
}
IBlueprintReader::ContentBrowserPathResult
CachingBlueprintReader::SetContentBrowserPath(std::string_view p) {
	return inner_->SetContentBrowserPath(p);
}
IBlueprintReader::WorldToScreenResult
CachingBlueprintReader::WorldToScreen(double x, double y, double z) {
	return inner_->WorldToScreen(x, y, z);
}
IBlueprintReader::ScreenToWorldResult
CachingBlueprintReader::ScreenToWorld(double x, double y, double d) {
	return inner_->ScreenToWorld(x, y, d);
}
IBlueprintReader::UiSnapshotResult
CachingBlueprintReader::UiSnapshot(std::string_view w, int d) {
	return inner_->UiSnapshot(w, d);
}
IBlueprintReader::UiSnapshotResult
CachingBlueprintReader::UiFind(std::string_view t, std::string_view r) {
	return inner_->UiFind(t, r);
}
IBlueprintReader::DesktopWindowsResult CachingBlueprintReader::ListDesktopWindows() {
	return inner_->ListDesktopWindows();
}
IBlueprintReader::GameFeaturesListResult CachingBlueprintReader::ListGameFeatures() {
	return inner_->ListGameFeatures();
}
IBlueprintReader::GameFeatureStateResult
CachingBlueprintReader::GetGameFeatureState(std::string_view p) {
	return inner_->GetGameFeatureState(p);
}
IBlueprintReader::PluginListResult CachingBlueprintReader::ListPlugins() {
	return inner_->ListPlugins();
}
IBlueprintReader::PluginDescriptorResult
CachingBlueprintReader::GetPluginDescriptor(std::string_view p) {
	return inner_->GetPluginDescriptor(p);
}
IBlueprintReader::PluginDependenciesResult
CachingBlueprintReader::GetPluginDependencies(std::string_view p) {
	return inner_->GetPluginDependencies(p);
}
IBlueprintReader::ActorAbilitiesResult
CachingBlueprintReader::ListActorAbilities(std::string_view a) {
	return inner_->ListActorAbilities(a);
}
IBlueprintReader::ActorTagsResult
CachingBlueprintReader::ListActorGameplayTags(std::string_view a) {
	return inner_->ListActorGameplayTags(a);
}
IBlueprintReader::ActorAttributesResult
CachingBlueprintReader::ListActorAttributes(std::string_view a) {
	return inner_->ListActorAttributes(a);
}
IBlueprintReader::ActorEffectsResult
CachingBlueprintReader::ListActorGameplayEffects(std::string_view a) {
	return inner_->ListActorGameplayEffects(a);
}
IBlueprintReader::BlueprintEditorStateResult
CachingBlueprintReader::GetBlueprintEditorState(std::string_view a) {
	return inner_->GetBlueprintEditorState(a);
}
IBlueprintReader::MaterialInstanceParamsResult
CachingBlueprintReader::GetMaterialInstanceParams(std::string_view a) {
	return inner_->GetMaterialInstanceParams(a);
}
IBlueprintReader::StaticMeshInfoResult
CachingBlueprintReader::GetStaticMeshInfo(std::string_view a) {
	return inner_->GetStaticMeshInfo(a);
}
IBlueprintReader::UmgEditorStateResult
CachingBlueprintReader::GetUmgEditorState(std::string_view a) {
	return inner_->GetUmgEditorState(a);
}
IBlueprintReader::MaterialEditorStateResult
CachingBlueprintReader::GetMaterialEditorState(std::string_view a) {
	return inner_->GetMaterialEditorState(a);
}
IBlueprintReader::MeshPreviewStateResult
CachingBlueprintReader::GetMeshPreviewState(std::string_view a) {
	return inner_->GetMeshPreviewState(a);
}
IBlueprintReader::CinematicCameraResult
CachingBlueprintReader::GetCinematicCamera() {
	return inner_->GetCinematicCamera();
}
IBlueprintReader::SequencerStateResult
CachingBlueprintReader::GetSequencerState(std::string_view a) {
	return inner_->GetSequencerState(a);
}
IBlueprintReader::AnimEditorStateResult
CachingBlueprintReader::GetAnimEditorState(std::string_view a) {
	return inner_->GetAnimEditorState(a);
}
IBlueprintReader::NiagaraModuleSelectionResult
CachingBlueprintReader::GetNiagaraModuleSelection(std::string_view a) {
	return inner_->GetNiagaraModuleSelection(a);
}
IBlueprintReader::CurveEditorSelectionResult
CachingBlueprintReader::GetCurveEditorSelection(std::string_view a) {
	return inner_->GetCurveEditorSelection(a);
}
IBlueprintReader::BufferVizModeResult CachingBlueprintReader::GetBufferVisualizationMode() {
	return inner_->GetBufferVisualizationMode();
}
IBlueprintReader::GizmoStateResult CachingBlueprintReader::GetGizmoState() {
	return inner_->GetGizmoState();
}
IBlueprintReader::ViewportRealtimeResult CachingBlueprintReader::GetViewportRealtime() {
	return inner_->GetViewportRealtime();
}
IBlueprintReader::ViewportCameraSettingsResult CachingBlueprintReader::GetViewportCameraSettings() {
	return inner_->GetViewportCameraSettings();
}
IBlueprintReader::SnappingSettingsResult CachingBlueprintReader::GetSnappingSettings() {
	return inner_->GetSnappingSettings();
}
IBlueprintReader::ActiveViewportResult CachingBlueprintReader::GetActiveViewport() {
	return inner_->GetActiveViewport();
}
IBlueprintReader::HiddenActorsResult CachingBlueprintReader::GetHiddenActors() {
	return inner_->GetHiddenActors();
}
IBlueprintReader::VisibleActorsResult
CachingBlueprintReader::GetVisibleActors(std::string_view f, double d) {
	return inner_->GetVisibleActors(f, d);
}
IBlueprintReader::SetViewModeResult
CachingBlueprintReader::SetViewMode(std::string_view m) {
	return inner_->SetViewMode(m);
}
IBlueprintReader::SetGizmoModeResult
CachingBlueprintReader::SetGizmoMode(std::string_view m) {
	return inner_->SetGizmoMode(m);
}
IBlueprintReader::SetViewportRealtimeResult
CachingBlueprintReader::SetViewportRealtime(bool e) {
	return inner_->SetViewportRealtime(e);
}
IBlueprintReader::SetActorVisibilityResult
CachingBlueprintReader::SetActorVisibility(std::string_view n, bool v) {
	return inner_->SetActorVisibility(n, v);
}
IBlueprintReader::HiddenLayersResult CachingBlueprintReader::GetHiddenLayers() {
	return inner_->GetHiddenLayers();
}
IBlueprintReader::SetLayerVisibilityResult
CachingBlueprintReader::SetLayerVisibility(std::string_view l, bool v) {
	return inner_->SetLayerVisibility(l, v);
}
IBlueprintReader::CameraBookmarksResult CachingBlueprintReader::GetCameraBookmarks() {
	return inner_->GetCameraBookmarks();
}
IBlueprintReader::GotoBookmarkResult CachingBlueprintReader::GotoCameraBookmark(int s) {
	return inner_->GotoCameraBookmark(s);
}
IBlueprintReader::HoverTargetResult CachingBlueprintReader::GetHoverTarget() {
	return inner_->GetHoverTarget();
}
IBlueprintReader::IsolateModeResult CachingBlueprintReader::GetIsolateMode() {
	return inner_->GetIsolateMode();
}
IBlueprintReader::AsyncCompileStateResult CachingBlueprintReader::GetAsyncCompileState() {
	return inner_->GetAsyncCompileState();
}
IBlueprintReader::ShaderCompileStateResult CachingBlueprintReader::GetShaderCompileState() {
	return inner_->GetShaderCompileState();
}
IBlueprintReader::CurrentLevelResult CachingBlueprintReader::GetCurrentLevel() {
	return inner_->GetCurrentLevel();
}
IBlueprintReader::LoadedLevelsResult CachingBlueprintReader::ListLoadedLevels() {
	return inner_->ListLoadedLevels();
}
IBlueprintReader::SourceControlProviderResult CachingBlueprintReader::GetSourceControlProvider() {
	return inner_->GetSourceControlProvider();
}
IBlueprintReader::AssetRegistryStateResult CachingBlueprintReader::GetAssetRegistryState() {
	return inner_->GetAssetRegistryState();
}
IBlueprintReader::DataLayerStatesResult CachingBlueprintReader::GetDataLayerStates() {
	return inner_->GetDataLayerStates();
}
IBlueprintReader::AutosaveStatusResult CachingBlueprintReader::GetAutosaveStatus() {
	return inner_->GetAutosaveStatus();
}
IBlueprintReader::RecoveryStateResult CachingBlueprintReader::GetRecoveryState() {
	return inner_->GetRecoveryState();
}
IBlueprintReader::SourceControlStatusResult
CachingBlueprintReader::GetSourceControlStatus(std::string_view p) {
	return inner_->GetSourceControlStatus(p);
}
IBlueprintReader::FileLockStatusResult
CachingBlueprintReader::GetFileLockStatus(std::string_view p) {
	return inner_->GetFileLockStatus(p);
}
IBlueprintReader::ActiveCultureResult CachingBlueprintReader::GetActiveCulture() {
	return inner_->GetActiveCulture();
}
IBlueprintReader::EditorThemeResult CachingBlueprintReader::GetEditorTheme() {
	return inner_->GetEditorTheme();
}
IBlueprintReader::MonitorInfoResult CachingBlueprintReader::GetMonitors() {
	return inner_->GetMonitors();
}
IBlueprintReader::LiveCodingStateResult CachingBlueprintReader::GetLiveCodingState() {
	return inner_->GetLiveCodingState();
}
IBlueprintReader::GameFeatureActionResult
CachingBlueprintReader::ActivateGameFeature(std::string_view p) {
	return inner_->ActivateGameFeature(p);
}
IBlueprintReader::GameFeatureActionResult
CachingBlueprintReader::DeactivateGameFeature(std::string_view p) {
	return inner_->DeactivateGameFeature(p);
}
IBlueprintReader::RecentAssetsResult CachingBlueprintReader::GetRecentlyOpenedAssets() {
	return inner_->GetRecentlyOpenedAssets();
}

// ----- Material authoring (pass-through; writes invalidate the asset) ---

std::vector<BPAssetSummary>
CachingBlueprintReader::ListMaterials(std::string_view path) {
	return inner_->ListMaterials(path);
}
IBlueprintReader::MaterialInfo
CachingBlueprintReader::ReadMaterial(std::string_view a) {
	return inner_->ReadMaterial(a);
}
IBlueprintReader::AddMaterialExpressionResult
CachingBlueprintReader::AddMaterialExpression(std::string_view a,
	std::string_view c, int x, int y) {
	auto out = inner_->AddMaterialExpression(a, c, x, y);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::ConnectMaterialResult
CachingBlueprintReader::ConnectMaterialExpressions(std::string_view a,
	std::string_view fn, std::string_view fp,
	std::string_view tn, std::string_view tp) {
	auto out = inner_->ConnectMaterialExpressions(a, fn, fp, tn, tp);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetMaterialParameterResult
CachingBlueprintReader::SetMaterialParameter(std::string_view a,
	std::string_view p, std::string_view v) {
	auto out = inner_->SetMaterialParameter(a, p, v);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetMIParameterResult
CachingBlueprintReader::SetMaterialInstanceParameter(std::string_view a,
	std::string_view p, std::string_view t, std::string_view v) {
	auto out = inner_->SetMaterialInstanceParameter(a, p, t, v);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::CompileMaterialResult
CachingBlueprintReader::CompileMaterial(std::string_view a) {
	auto out = inner_->CompileMaterial(a);
	InvalidateAsset(a);
	return out;
}

// ----- UMG widget authoring (pass-through; writes invalidate) -----------

IBlueprintReader::WidgetBlueprintInfo
CachingBlueprintReader::ReadWidgetBlueprint(std::string_view a) {
	return inner_->ReadWidgetBlueprint(a);
}
IBlueprintReader::AddWidgetResult
CachingBlueprintReader::AddWidget(std::string_view a, std::string_view p,
	std::string_view c, std::string_view n) {
	auto out = inner_->AddWidget(a, p, c, n);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetWidgetPropertyResult
CachingBlueprintReader::SetWidgetProperty(std::string_view a, std::string_view w,
	std::string_view p, std::string_view v) {
	auto out = inner_->SetWidgetProperty(a, w, p, v);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::BindWidgetEventResult
CachingBlueprintReader::BindWidgetEvent(std::string_view a, std::string_view w,
	std::string_view e, std::string_view h) {
	auto out = inner_->BindWidgetEvent(a, w, e, h);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::CompileWidgetBlueprintResult
CachingBlueprintReader::CompileWidgetBlueprint(std::string_view a) {
	auto out = inner_->CompileWidgetBlueprint(a);
	InvalidateAsset(a);
	return out;
}

// ----- Behavior Tree (pass-through; writes invalidate) ------------------

std::vector<BPAssetSummary>
CachingBlueprintReader::ListBehaviorTrees(std::string_view p) {
	return inner_->ListBehaviorTrees(p);
}
IBlueprintReader::BehaviorTreeInfo
CachingBlueprintReader::ReadBehaviorTree(std::string_view a) {
	return inner_->ReadBehaviorTree(a);
}
IBlueprintReader::AddBTNodeResult
CachingBlueprintReader::AddBTNode(std::string_view a, std::string_view p,
	std::string_view k, std::string_view c) {
	auto out = inner_->AddBTNode(a, p, k, c);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetBTNodePropertyResult
CachingBlueprintReader::SetBTNodeProperty(std::string_view a, std::string_view n,
	std::string_view p, std::string_view v) {
	auto out = inner_->SetBTNodeProperty(a, n, p, v);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::CompileBehaviorTreeResult
CachingBlueprintReader::CompileBehaviorTree(std::string_view a) {
	auto out = inner_->CompileBehaviorTree(a);
	InvalidateAsset(a);
	return out;
}

// ----- DataAsset (pass-through; writes invalidate) ----------------------

std::vector<BPAssetSummary>
CachingBlueprintReader::ListDataAssets(std::string_view p) {
	return inner_->ListDataAssets(p);
}
IBlueprintReader::DataAssetInfo
CachingBlueprintReader::ReadDataAsset(std::string_view a) {
	return inner_->ReadDataAsset(a);
}
IBlueprintReader::CreateDataAssetResult
CachingBlueprintReader::CreateDataAsset(std::string_view a, std::string_view c) {
	auto out = inner_->CreateDataAsset(a, c);
	InvalidateAsset("");  // global list cache may change
	return out;
}
IBlueprintReader::SetDataAssetPropertyResult
CachingBlueprintReader::SetDataAssetProperty(std::string_view a,
	std::string_view p, std::string_view v) {
	auto out = inner_->SetDataAssetProperty(a, p, v);
	InvalidateAsset(a);
	return out;
}

// ----- StateTree (pass-through; writes invalidate) ----------------------

std::vector<BPAssetSummary>
CachingBlueprintReader::ListStateTrees(std::string_view p) {
	return inner_->ListStateTrees(p);
}
IBlueprintReader::StateTreeInfo
CachingBlueprintReader::ReadStateTree(std::string_view a) {
	return inner_->ReadStateTree(a);
}
IBlueprintReader::AddStateTreeStateResult
CachingBlueprintReader::AddStateTreeState(std::string_view a,
	std::string_view p, std::string_view n) {
	auto out = inner_->AddStateTreeState(a, p, n);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetStateTreeTransitionResult
CachingBlueprintReader::SetStateTreeTransition(std::string_view a,
	std::string_view f, std::string_view t, std::string_view tr) {
	auto out = inner_->SetStateTreeTransition(a, f, t, tr);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::CompileStateTreeResult
CachingBlueprintReader::CompileStateTree(std::string_view a) {
	auto out = inner_->CompileStateTree(a);
	InvalidateAsset(a);
	return out;
}

// ----- Stage 3 (pass-through; transient editor state, never cached) ----

IBlueprintReader::StartProfileResult
CachingBlueprintReader::StartProfile(std::string_view m) { return inner_->StartProfile(m); }
IBlueprintReader::StopProfileResult
CachingBlueprintReader::StopProfile() { return inner_->StopProfile(); }
IBlueprintReader::StatGroupResult
CachingBlueprintReader::GetStats(std::string_view g) { return inner_->GetStats(g); }
IBlueprintReader::ScreenshotResult
CachingBlueprintReader::TakeScreenshot(std::string_view d, int w, int h) {
	return inner_->TakeScreenshot(d, w, h);
}
IBlueprintReader::CookResult
CachingBlueprintReader::CookContent(std::string_view p) { return inner_->CookContent(p); }
IBlueprintReader::CookResult
CachingBlueprintReader::PackageProject(std::string_view p, std::string_view o) {
	return inner_->PackageProject(p, o);
}
IBlueprintReader::ClassInfo
CachingBlueprintReader::IntrospectClass(std::string_view c) { return inner_->IntrospectClass(c); }
IBlueprintReader::FindClassResult
CachingBlueprintReader::FindClass(std::string_view q) { return inner_->FindClass(q); }
std::vector<IBlueprintReader::ClassFunctionInfo>
CachingBlueprintReader::ListFunctions(std::string_view c) { return inner_->ListFunctions(c); }
IBlueprintReader::FocusActorResult
CachingBlueprintReader::FocusActor(std::string_view a) { return inner_->FocusActor(a); }
IBlueprintReader::SetCameraResult
CachingBlueprintReader::SetCameraTransform(double lx, double ly, double lz,
	double rp, double ry, double rr) {
	return inner_->SetCameraTransform(lx, ly, lz, rp, ry, rr);
}
IBlueprintReader::ViewportScreenshotResult
CachingBlueprintReader::TakeViewportScreenshot(std::string_view d) {
	return inner_->TakeViewportScreenshot(d);
}
IBlueprintReader::SetShowFlagResult
CachingBlueprintReader::SetShowFlag(std::string_view f, bool e) {
	return inner_->SetShowFlag(f, e);
}

// ----- Stage 4 (pass-through; writes invalidate the asset) -------------

std::vector<BPAssetSummary>
CachingBlueprintReader::ListNiagaraSystems(std::string_view p) { return inner_->ListNiagaraSystems(p); }
IBlueprintReader::NiagaraSystemInfo
CachingBlueprintReader::ReadNiagaraSystem(std::string_view a) { return inner_->ReadNiagaraSystem(a); }
IBlueprintReader::CreateNiagaraSystemResult
CachingBlueprintReader::CreateNiagaraSystem(std::string_view a) {
	auto out = inner_->CreateNiagaraSystem(a);
	InvalidateAsset("");
	return out;
}
IBlueprintReader::SetNiagaraParameterResult
CachingBlueprintReader::SetNiagaraParameter(std::string_view a,
	std::string_view p, std::string_view v) {
	auto out = inner_->SetNiagaraParameter(a, p, v);
	InvalidateAsset(a);
	return out;
}
std::vector<BPAssetSummary>
CachingBlueprintReader::ListLevelSequences(std::string_view p) { return inner_->ListLevelSequences(p); }
IBlueprintReader::LevelSequenceInfo
CachingBlueprintReader::ReadLevelSequence(std::string_view a) { return inner_->ReadLevelSequence(a); }
IBlueprintReader::AddSequenceTrackResult
CachingBlueprintReader::AddSequenceTrack(std::string_view a,
	std::string_view c, std::string_view n) {
	auto out = inner_->AddSequenceTrack(a, c, n);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::SetSequencePlaybackRangeResult
CachingBlueprintReader::SetSequencePlaybackRange(std::string_view a,
	double s, double e) {
	auto out = inner_->SetSequencePlaybackRange(a, s, e);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::GameplayTagListResult
CachingBlueprintReader::ListGameplayTags(std::string_view f) { return inner_->ListGameplayTags(f); }
IBlueprintReader::AddGameplayTagResult
CachingBlueprintReader::AddGameplayTag(std::string_view t, std::string_view c) {
	auto out = inner_->AddGameplayTag(t, c);
	InvalidateAsset("");  // tag registry is project-global
	return out;
}
IBlueprintReader::AbilitySetInfo
CachingBlueprintReader::ReadAbilitySet(std::string_view a) { return inner_->ReadAbilitySet(a); }
std::vector<BPAssetSummary>
CachingBlueprintReader::ListAnimBlueprints(std::string_view p) { return inner_->ListAnimBlueprints(p); }
IBlueprintReader::AnimBlueprintInfo
CachingBlueprintReader::ReadAnimBlueprint(std::string_view a) { return inner_->ReadAnimBlueprint(a); }
IBlueprintReader::AddAnimStateResult
CachingBlueprintReader::AddAnimState(std::string_view a, std::string_view m, std::string_view n) {
	auto out = inner_->AddAnimState(a, m, n);
	InvalidateAsset(a);
	return out;
}
IBlueprintReader::CompileAnimBlueprintResult
CachingBlueprintReader::CompileAnimBlueprint(std::string_view a) {
	auto out = inner_->CompileAnimBlueprint(a);
	InvalidateAsset(a);
	return out;
}

// ============================================================================
// Factory helper
// ============================================================================

std::unique_ptr<IBlueprintReader> WrapWithCache(
	std::unique_ptr<IBlueprintReader> inner, std::chrono::seconds ttl,
	std::filesystem::path projectDir) {
	if (ttl <= std::chrono::seconds(0))
	{
		return inner;
	}
	return std::make_unique<CachingBlueprintReader>(
		std::move(inner), ttl, std::move(projectDir));
}

}    // namespace bpr::backends
