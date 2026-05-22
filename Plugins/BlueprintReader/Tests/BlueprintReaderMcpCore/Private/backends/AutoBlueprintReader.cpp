#include "backends/AutoBlueprintReader.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif    // WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "Ws2_32.lib")
#else    // defined(_WIN32)
	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif    // defined(_WIN32)

namespace bpr::backends {

namespace auto_blueprint_reader_detail {

// Re-read a handshake file (same shape as BackendFactory.cpp's
// ReadHandshakeFile, but Auto re-reads on every probe so a
// freshly-launched editor or daemon's port + token are picked up
// without needing an MCP server restart). The filename varies:
// `bp-reader-live.json` for the editor, `bp-reader-cmdlet.json` for
// the commandlet daemon.
struct AutoHandshake {
	std::string host;
	int         port = 0;
	std::string token;
};
std::optional<AutoHandshake> ReadHandshakeFile(const std::filesystem::path& uproject,
											   const char* filename) {
	if (uproject.empty())
	{
		return std::nullopt;
	}
	auto path = uproject.parent_path() / "Saved" / filename;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		return std::nullopt;
	}
	std::ifstream f(path);
	if (!f)
	{
		return std::nullopt;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	nlohmann::json j;
	try {
		j = nlohmann::json::parse(ss.str());
	} catch (...) { return std::nullopt; }
	AutoHandshake hs;
	hs.host  = j.value("host",  std::string("127.0.0.1"));
	hs.port  = j.value("port",  0);
	hs.token = j.value("token", std::string());
	if (hs.port <= 0 || hs.token.empty())
	{
		return std::nullopt;
	}
	return hs;
}

// Back-compat overload for callers that just want the live handshake.
std::optional<AutoHandshake> ReadHandshake(const std::filesystem::path& uproject) {
	return ReadHandshakeFile(uproject, "bp-reader-live.json");
}

// One-shot TCP connect with a short timeout — confirms the editor's
// listener is actually accepting connections (not just that the
// handshake file exists from a crashed editor). We don't do the
// auth handshake here; SocketBlueprintReader does its own on first op.
bool TcpProbe(const std::string& host, int port,
			  std::chrono::milliseconds timeout) {
#if defined(_WIN32)
	WSADATA wsa;
	bool wsaInited = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
	auto cleanup = [wsaInited]() { if (wsaInited) WSACleanup(); };
#else    // defined(_WIN32)
	auto cleanup = []() {};
#endif    // defined(_WIN32)

#if defined(_WIN32)
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) { cleanup(); return false; }
	auto closeSock = [&]() { ::closesocket(s); cleanup(); };
	u_long nb = 1; ::ioctlsocket(s, FIONBIO, &nb);
#else    // defined(_WIN32)
	int s = ::socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { cleanup(); return false; }
	auto closeSock = [&]() { ::close(s); cleanup(); };
	int flags = ::fcntl(s, F_GETFL, 0);
	::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif    // defined(_WIN32)

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(static_cast<uint16_t>(port));
#if defined(_WIN32)
	inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
#else    // defined(_WIN32)
	inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
#endif    // defined(_WIN32)

	int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (rc == 0) { closeSock(); return true; }

	// Non-blocking connect: wait for writability up to timeout.
	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(s, &wfds);
	timeval tv;
	tv.tv_sec  = static_cast<long>(timeout.count() / 1000);
	tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
	rc = ::select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
	if (rc <= 0) { closeSock(); return false; }

	// Connect either succeeded or got refused — distinguish via SO_ERROR.
	int err = 0;
#if defined(_WIN32)
	int errLen = sizeof(err);
	::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen);
#else    // defined(_WIN32)
	socklen_t errLen = sizeof(err);
	::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errLen);
#endif    // defined(_WIN32)
	closeSock();
	return err == 0;
}

}    // namespace auto_blueprint_reader_detail
using namespace auto_blueprint_reader_detail;

AutoBlueprintReader::AutoBlueprintReader(Config cfg) : cfg_(std::move(cfg)) {
	// Both backends are constructed lazily. Commandlet validation is
	// expensive (it checks for the editor exe on disk) and routes
	// where Live is always reachable would never need it. Live is
	// also lazy so we only build it after the first probe-success.
	if (cfg_.prewarmCommandlet) {
		// Caller asked for prewarm — eagerly construct + warm. Errors
		// here surface to the caller (matches the old explicit-
		// commandlet semantics).
		commandlet_ = std::make_unique<CommandletBlueprintReader>(cfg_.commandletConfig);
		commandlet_->Prewarm();
	}
}

AutoBlueprintReader::~AutoBlueprintReader() = default;

CommandletBlueprintReader& AutoBlueprintReader::EnsureCommandlet() {
	if (!commandlet_) {
		commandlet_ = std::make_unique<CommandletBlueprintReader>(cfg_.commandletConfig);
	}
	return *commandlet_;
}

std::unique_ptr<SocketBlueprintReader> AutoBlueprintReader::TryBuildLive() {
	// Env-var values (passed through cfg_) take precedence over the
	// handshake file. If we have everything from env, build directly.
	SocketBlueprintReader::Config lc;
	lc.host  = cfg_.liveHost.empty() ? "127.0.0.1" : cfg_.liveHost;
	lc.port  = cfg_.livePort;
	lc.token = cfg_.liveToken;

	// Fill missing fields from the handshake file. The MCP server
	// re-reads the file (rather than caching from startup) so that an
	// editor restart with a fresh ephemeral port + token gets picked
	// up without restarting the MCP server.
	if (lc.port == 0 || lc.token.empty()) {
		if (auto hs = ReadHandshake(cfg_.uproject)) {
			if (lc.port == 0)
			{
				lc.port  = hs->port;
			}
			if (lc.token.empty())
			{
				lc.token = hs->token;
			}
			if (lc.host == "127.0.0.1" && !hs->host.empty())
			{
				lc.host = hs->host;
			}
		}
	}
	if (lc.port == 0 || lc.token.empty())
	{
		return nullptr;
	}
	// Same self-refresh wiring as the static `live` backend (issue #9
	// recovery). Auto's per-call probe also handles editor-restart
	// recovery at the outer layer, but inner-layer refresh keeps a
	// currently-live SocketBlueprintReader usable across an editor
	// restart that happens between the probe and the next op.
	if (!cfg_.uproject.empty()) {
		lc.handshakeFilePath =
			(cfg_.uproject.parent_path() / "Saved" / "bp-reader-live.json").string();
		lc.projectPath = cfg_.uproject.string();
	}
	return std::make_unique<SocketBlueprintReader>(std::move(lc));
}

std::unique_ptr<SocketBlueprintReader> AutoBlueprintReader::TryBuildCmdlet() {
	// Mirror of TryBuildLive but for the commandlet daemon's handshake
	// file. Unlike the live path, env-var overrides don't apply here —
	// the cmdlet daemon is project-local and there's no user-visible
	// BP_READER_CMDLET_PORT/TOKEN knob. The handshake file is the only
	// source of truth.
	auto hs = ReadHandshakeFile(cfg_.uproject, "bp-reader-cmdlet.json");
	if (!hs)
	{
		return nullptr;
	}

	SocketBlueprintReader::Config sc;
	sc.host  = hs->host.empty() ? "127.0.0.1" : hs->host;
	sc.port  = hs->port;
	sc.token = hs->token;
	// Wire the handshake-file path through so the socket reader can
	// self-refresh on connect-refused / auth-fail (issue #9 pattern,
	// also applies when the daemon restarts).
	if (!cfg_.uproject.empty()) {
		sc.handshakeFilePath =
			(cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet.json").string();
		sc.projectPath = cfg_.uproject.string();
	}
	if (sc.port <= 0 || sc.token.empty())
	{
		return nullptr;
	}
	return std::make_unique<SocketBlueprintReader>(std::move(sc));
}

void AutoBlueprintReader::Probe() {
	lastProbe_ = std::chrono::steady_clock::now();

	// ----- 1. Try live (running editor with the BlueprintReader plugin loaded).
	if (auto liveCandidate = TryBuildLive()) {
		// The candidate has a port + token; confirm something is
		// actually listening before we commit. A stale handshake file
		// from a crashed editor would otherwise route us to a broken
		// Live until the file ages out.
		std::string probeHost;
		int probePort = 0;
		auto hs = ReadHandshake(cfg_.uproject);
		if (cfg_.livePort != 0) {
			probePort = cfg_.livePort;
			probeHost = cfg_.liveHost.empty() ? "127.0.0.1" : cfg_.liveHost;
		} else if (hs) {
			probePort = hs->port;
			probeHost = hs->host.empty() ? "127.0.0.1" : hs->host;
		}
		if (probePort > 0 &&
			TcpProbe(probeHost, probePort, cfg_.probeConnectTimeout)) {
			// If we already have a live_ reader, keep it — its socket
			// may be hot. SocketBlueprintReader doesn't expose its
			// config publicly, so we rebuild on every probe transition.
			// Cost: one TCP handshake per editor open/close, not per call.
			if (!live_)
			{
				live_ = std::move(liveCandidate);
			}
			// Drop the cmdlet-socket route (live wins).
			cmdletSocket_.reset();
			route_ = Route::Live;
			return;
		}
	}
	// No reachable live — drop any cached live reader so it's rebuilt
	// on the next probe-success.
	live_.reset();

	// ----- 2. Try the commandlet daemon's TCP listener.
	if (auto cmdletCandidate = TryBuildCmdlet()) {
		auto hs = ReadHandshakeFile(cfg_.uproject, "bp-reader-cmdlet.json");
		if (hs &&
			TcpProbe(hs->host.empty() ? "127.0.0.1" : hs->host,
					 hs->port, cfg_.probeConnectTimeout)) {
			if (!cmdletSocket_)
			{
				cmdletSocket_ = std::move(cmdletCandidate);
			}
			route_ = Route::CmdletSocket;
			return;
		}
	}
	cmdletSocket_.reset();

	// ----- 3. No daemons available — fall back to spawning a commandlet.
	route_ = Route::Commandlet;
}

IBlueprintReader& AutoBlueprintReader::Pick() {
	auto now = std::chrono::steady_clock::now();
	if (now - lastProbe_ >= cfg_.probeTtl) {
		Probe();
	}
	if (route_ == Route::Live && live_)
	{
		return *live_;
	}
	if (route_ == Route::CmdletSocket && cmdletSocket_)
	{
		return *cmdletSocket_;
	}
	return EnsureCommandlet();
}

std::string AutoBlueprintReader::SelectBackendForTesting() {
	std::lock_guard<std::mutex> lock(mu_);
	// Force a fresh probe.
	lastProbe_ = std::chrono::steady_clock::time_point{};
	Probe();
	// CmdletSocket is reported as "commandlet" — semantically it routes
	// to the commandlet path, just via the daemon's TCP listener
	// instead of spawning a new commandlet. Test cases that care about
	// the distinction should be added alongside this name.
	switch (route_) {
		case Route::Live:         return "live";
		case Route::CmdletSocket: return "commandlet";
		case Route::Commandlet:   return "commandlet";
	}
	return "commandlet";
}

// ===== Forwarders =========================================================
// All of these grab the mutex, pick a backend, then delegate. The mutex
// keeps probe state consistent; it does NOT serialize the calls
// themselves (the inner backends already serialize their own access
// where needed). On Pick, we may rebuild live_ / probe — that's the
// blocking section.

#define FORWARD(method, ...)                          \
	do {                                              \
		std::lock_guard<std::mutex> lock(mu_);        \
		return Pick().method(__VA_ARGS__);            \
	} while (0)

#define FORWARD_VOID(method, ...)                     \
	do {                                              \
		std::lock_guard<std::mutex> lock(mu_);        \
		Pick().method(__VA_ARGS__);                   \
	} while (0)

std::vector<BPAssetSummary> AutoBlueprintReader::ListBlueprints(std::string_view p) {
	FORWARD(ListBlueprints, p);
}
BPMetadata AutoBlueprintReader::ReadBlueprint(std::string_view a) {
	FORWARD(ReadBlueprint, a);
}
BPGraph AutoBlueprintReader::GetGraph(std::string_view a, std::string_view g) {
	FORWARD(GetGraph, a, g);
}
BPFunction AutoBlueprintReader::GetFunction(std::string_view a, std::string_view f) {
	FORWARD(GetFunction, a, f);
}
std::vector<BPVariable> AutoBlueprintReader::ListVariables(std::string_view a) {
	FORWARD(ListVariables, a);
}
std::vector<BPComponent> AutoBlueprintReader::GetComponents(std::string_view a) {
	FORWARD(GetComponents, a);
}
std::vector<BPNode> AutoBlueprintReader::FindNode(std::string_view a,
												  std::string_view q,
												  std::string_view k) {
	FORWARD(FindNode, a, q, k);
}

void AutoBlueprintReader::AddVariable(std::string_view a, std::string_view n,
									  const BPPinType& t, std::string_view d,
									  std::string_view c, bool r, bool e) {
	FORWARD_VOID(AddVariable, a, n, t, d, c, r, e);
}
void AutoBlueprintReader::SetNodePosition(std::string_view a, std::string_view g,
										  std::string_view n, int x, int y) {
	FORWARD_VOID(SetNodePosition, a, g, n, x, y);
}
void AutoBlueprintReader::DeleteNode(std::string_view a, std::string_view g,
									 std::string_view n) {
	FORWARD_VOID(DeleteNode, a, g, n);
}
std::string AutoBlueprintReader::AddNode(std::string_view a, std::string_view g,
										 std::string_view k, int x, int y,
										 const std::map<std::string, std::string,
														std::less<>>& extras) {
	FORWARD(AddNode, a, g, k, x, y, extras);
}
void AutoBlueprintReader::WirePins(std::string_view a, std::string_view g,
								   std::string_view fn, std::string_view fp,
								   std::string_view tn, std::string_view tp) {
	FORWARD_VOID(WirePins, a, g, fn, fp, tn, tp);
}
void AutoBlueprintReader::DeleteVariable(std::string_view a, std::string_view n) {
	FORWARD_VOID(DeleteVariable, a, n);
}
void AutoBlueprintReader::RenameVariable(std::string_view a, std::string_view o,
										 std::string_view n) {
	FORWARD_VOID(RenameVariable, a, o, n);
}
IBlueprintReader::AddFunctionResult
AutoBlueprintReader::AddFunction(std::string_view a, std::string_view n) {
	FORWARD(AddFunction, a, n);
}
void AutoBlueprintReader::AddFunctionInput(std::string_view a, std::string_view fn,
										   std::string_view p, const BPPinType& t) {
	FORWARD_VOID(AddFunctionInput, a, fn, p, t);
}
void AutoBlueprintReader::AddFunctionOutput(std::string_view a, std::string_view fn,
											std::string_view p, const BPPinType& t) {
	FORWARD_VOID(AddFunctionOutput, a, fn, p, t);
}
void AutoBlueprintReader::DeleteFunction(std::string_view a, std::string_view n) {
	FORWARD_VOID(DeleteFunction, a, n);
}
void AutoBlueprintReader::SetVariableDefault(std::string_view a, std::string_view n,
											 std::string_view d) {
	FORWARD_VOID(SetVariableDefault, a, n, d);
}
IBlueprintReader::CreateBlueprintResult
AutoBlueprintReader::CreateBlueprint(std::string_view a, std::string_view p) {
	FORWARD(CreateBlueprint, a, p);
}
void AutoBlueprintReader::SetPinDefault(std::string_view a, std::string_view g,
										std::string_view n, std::string_view p,
										std::string_view v) {
	FORWARD_VOID(SetPinDefault, a, g, n, p, v);
}
void AutoBlueprintReader::RetypeVariable(std::string_view a, std::string_view n,
										 const BPPinType& t) {
	FORWARD_VOID(RetypeVariable, a, n, t);
}
void AutoBlueprintReader::SetVariableCategory(std::string_view a, std::string_view n,
											  std::string_view c) {
	FORWARD_VOID(SetVariableCategory, a, n, c);
}
IBlueprintReader::DuplicateBlueprintResult
AutoBlueprintReader::DuplicateBlueprint(std::string_view s, std::string_view d) {
	FORWARD(DuplicateBlueprint, s, d);
}
IBlueprintReader::WriteGeneratedSourceResult
AutoBlueprintReader::WriteGeneratedSource(std::string_view p, std::string_view c,
										  bool cd) {
	FORWARD(WriteGeneratedSource, p, c, cd);
}
nlohmann::json AutoBlueprintReader::StructuralDiff(
	std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
	FORWARD(StructuralDiff, a, b, opts);
}

void AutoBlueprintReader::BeginBatch() {
	FORWARD_VOID(BeginBatch);
}
nlohmann::json AutoBlueprintReader::EndBatch(bool skipCompile) {
	FORWARD(EndBatch, skipCompile);
}
IBlueprintReader::ProjectMetadata
AutoBlueprintReader::GetProjectMetadata() {
	FORWARD(GetProjectMetadata);
}
IBlueprintReader::AssetRegistryListResult
AutoBlueprintReader::ListAssets(std::string_view path, bool recursive) {
	FORWARD(ListAssets, path, recursive);
}
IBlueprintReader::AssetRegistryListResult
AutoBlueprintReader::FindAsset(std::string_view query, std::string_view path) {
	FORWARD(FindAsset, query, path);
}

// ----- Phase 8 EA-pull Wave 1 ----------------------------------------------

IBlueprintReader::OpenAssetsResult AutoBlueprintReader::ListOpenAssets() {
	FORWARD(ListOpenAssets);
}
IBlueprintReader::ActiveAssetResult AutoBlueprintReader::GetActiveAsset() {
	FORWARD(GetActiveAsset);
}
IBlueprintReader::CompileStatusResult
AutoBlueprintReader::GetCompileStatus(std::string_view a) {
	FORWARD(GetCompileStatus, a);
}
IBlueprintReader::DirtyPackagesResult AutoBlueprintReader::GetDirtyPackages() {
	FORWARD(GetDirtyPackages);
}
IBlueprintReader::FocusedWindowResult AutoBlueprintReader::GetFocusedWindow() {
	FORWARD(GetFocusedWindow);
}
IBlueprintReader::PieStateResult AutoBlueprintReader::GetPieState() {
	FORWARD(GetPieState);
}
IBlueprintReader::ModalStateResult AutoBlueprintReader::GetModalState() {
	FORWARD(GetModalState);
}
IBlueprintReader::EditorModesResult AutoBlueprintReader::GetActiveEditorMode() {
	FORWARD(GetActiveEditorMode);
}
IBlueprintReader::FocusedWidgetResult AutoBlueprintReader::GetFocusedWidget() {
	FORWARD(GetFocusedWidget);
}
IBlueprintReader::OpenAssetEditorResult
AutoBlueprintReader::OpenAssetEditor(std::string_view a) {
	FORWARD(OpenAssetEditor, a);
}
IBlueprintReader::CloseAssetEditorResult
AutoBlueprintReader::CloseAssetEditor(std::string_view a) {
	FORWARD(CloseAssetEditor, a);
}
IBlueprintReader::CameraTransformResult AutoBlueprintReader::GetCameraTransform() {
	FORWARD(GetCameraTransform);
}
IBlueprintReader::ViewModeResult AutoBlueprintReader::GetViewMode() {
	FORWARD(GetViewMode);
}
IBlueprintReader::ShowFlagsResult AutoBlueprintReader::GetShowFlags() {
	FORWARD(GetShowFlags);
}
IBlueprintReader::SelectedComponentsResult AutoBlueprintReader::GetSelectedComponents() {
	FORWARD(GetSelectedComponents);
}
IBlueprintReader::ContentBrowserSelectionResult AutoBlueprintReader::GetSelectedAssets() {
	FORWARD(GetSelectedAssets);
}
IBlueprintReader::ContentBrowserSelectionResult
AutoBlueprintReader::SetSelectedAssets(const std::vector<std::string>& a) {
	FORWARD(SetSelectedAssets, a);
}
IBlueprintReader::ContentBrowserFoldersResult AutoBlueprintReader::GetSelectedFolders() {
	FORWARD(GetSelectedFolders);
}
IBlueprintReader::ContentBrowserPathResult AutoBlueprintReader::GetContentBrowserPath() {
	FORWARD(GetContentBrowserPath);
}
IBlueprintReader::ContentBrowserPathResult
AutoBlueprintReader::SetContentBrowserPath(std::string_view p) {
	FORWARD(SetContentBrowserPath, p);
}
IBlueprintReader::WorldToScreenResult
AutoBlueprintReader::WorldToScreen(double x, double y, double z) {
	FORWARD(WorldToScreen, x, y, z);
}
IBlueprintReader::ScreenToWorldResult
AutoBlueprintReader::ScreenToWorld(double x, double y, double d) {
	FORWARD(ScreenToWorld, x, y, d);
}
IBlueprintReader::UiSnapshotResult
AutoBlueprintReader::UiSnapshot(std::string_view w, int d) {
	FORWARD(UiSnapshot, w, d);
}
IBlueprintReader::UiSnapshotResult
AutoBlueprintReader::UiFind(std::string_view t, std::string_view r) {
	FORWARD(UiFind, t, r);
}
IBlueprintReader::DesktopWindowsResult AutoBlueprintReader::ListDesktopWindows() {
	FORWARD(ListDesktopWindows);
}
IBlueprintReader::GameFeaturesListResult AutoBlueprintReader::ListGameFeatures() {
	FORWARD(ListGameFeatures);
}
IBlueprintReader::GameFeatureStateResult
AutoBlueprintReader::GetGameFeatureState(std::string_view p) {
	FORWARD(GetGameFeatureState, p);
}
IBlueprintReader::PluginListResult AutoBlueprintReader::ListPlugins() {
	FORWARD(ListPlugins);
}
IBlueprintReader::PluginDescriptorResult
AutoBlueprintReader::GetPluginDescriptor(std::string_view p) {
	FORWARD(GetPluginDescriptor, p);
}
IBlueprintReader::PluginDependenciesResult
AutoBlueprintReader::GetPluginDependencies(std::string_view p) {
	FORWARD(GetPluginDependencies, p);
}
IBlueprintReader::ActorAbilitiesResult
AutoBlueprintReader::ListActorAbilities(std::string_view a) {
	FORWARD(ListActorAbilities, a);
}
IBlueprintReader::ActorTagsResult
AutoBlueprintReader::ListActorGameplayTags(std::string_view a) {
	FORWARD(ListActorGameplayTags, a);
}
IBlueprintReader::ActorAttributesResult
AutoBlueprintReader::ListActorAttributes(std::string_view a) {
	FORWARD(ListActorAttributes, a);
}
IBlueprintReader::ActorEffectsResult
AutoBlueprintReader::ListActorGameplayEffects(std::string_view a) {
	FORWARD(ListActorGameplayEffects, a);
}

nlohmann::json AutoBlueprintReader::ShutdownDaemon() {
	// Always route to commandlet — Live has no daemon to shut down,
	// and the user calling shutdown_daemon explicitly wants to release
	// the editor-daemon process locks (not affect the live editor).
	// If commandlet was never constructed there's nothing to tear down;
	// mirror IBlueprintReader's default {ok:true, was_running:false}.
	std::lock_guard<std::mutex> lock(mu_);
	if (!commandlet_) {
		return nlohmann::json{
			{"ok", true},
			{"was_running", false},
			{"hint", "auto backend never spawned a commandlet (live editor "
					 "covered every call this session)"},
		};
	}
	return commandlet_->ShutdownDaemon();
}

}    // namespace bpr::backends
