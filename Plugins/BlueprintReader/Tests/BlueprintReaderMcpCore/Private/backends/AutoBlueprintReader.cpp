#include "backends/AutoBlueprintReader.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
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
			// Release any commandlet daemon WE spawned during an editor-down
			// window so it stops contending with the now-running editor for
			// the project (two UE processes on one project is the hazard).
			// No-op if we only attached to a shared daemon or never spawned
			// one; harmless to call each live probe (no-op once torn down).
			// Without this the daemon lingers until its idle timeout (~5 min).
			if (commandlet_)
			{
				commandlet_->ShutdownSpawnedDaemon();
			}
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

#define FORWARD(method, ...) \
	do { \
		std::lock_guard<std::mutex> lock(mu_); \
		IBlueprintReader& picked = Pick(); \
		if (route_ == Route::Commandlet) { \
			return picked.method(__VA_ARGS__); \
		} \
		try { \
			return picked.method(__VA_ARGS__); \
		} catch (const SocketTransportError& bprTransportErr) { \
			return FallBackToCommandlet(bprTransportErr).method(__VA_ARGS__); \
		} \
	} while (0)

#define FORWARD_VOID(method, ...) \
	do { \
		std::lock_guard<std::mutex> lock(mu_); \
		IBlueprintReader& picked = Pick(); \
		if (route_ == Route::Commandlet) { \
			picked.method(__VA_ARGS__); \
		} else { \
			try { \
				picked.method(__VA_ARGS__); \
			} catch (const SocketTransportError& bprTransportErr) { \
				FallBackToCommandlet(bprTransportErr).method(__VA_ARGS__); \
			} \
		} \
	} while (0)

IBlueprintReader& AutoBlueprintReader::FallBackToCommandlet(const SocketTransportError& e) {
	// The probed socket route (live editor or daemon) connected but then
	// failed at the transport/auth layer — a stale token, a zombie listener,
	// or an editor dying mid-call. Surfacing this on every call strands the
	// session (the repeated auth_fail users hit). Drop the dead socket route
	// and serve this call (plus the next probeTtl window) from the commandlet;
	// the cooldown stops us re-probing straight back onto the broken socket,
	// and once it elapses the next probe re-evaluates — so we recover on our
	// own if the editor comes back.
	std::fprintf(stderr,
		"[bp-reader-mcp][auto] socket route transport error, falling back to "
		"commandlet: %s\n", e.what());
	live_.reset();
	cmdletSocket_.reset();
	route_ = Route::Commandlet;
	lastProbe_ = std::chrono::steady_clock::now();
	return EnsureCommandlet();
}

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
IBlueprintReader::PythonResult AutoBlueprintReader::RunPythonScript(std::string_view code) {
	FORWARD(RunPythonScript, code);
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
AutoBlueprintReader::CreateBlueprint(std::string_view a, std::string_view p,
									 std::string_view bt) {
	FORWARD(CreateBlueprint, a, p, bt);
}
IBlueprintReader::CloneGraphResult
AutoBlueprintReader::CloneGraph(std::string_view s, std::string_view t,
								std::string_view g) {
	FORWARD(CloneGraph, s, t, g);
}
void AutoBlueprintReader::ImplementInterface(std::string_view a, std::string_view i) {
	FORWARD_VOID(ImplementInterface, a, i);
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
nlohmann::json AutoBlueprintReader::ReadActorInstance(std::string_view assetPath) {
	FORWARD(ReadActorInstance, assetPath);
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
IBlueprintReader::BlueprintEditorStateResult
AutoBlueprintReader::GetBlueprintEditorState(std::string_view a) {
	FORWARD(GetBlueprintEditorState, a);
}
IBlueprintReader::MaterialInstanceParamsResult
AutoBlueprintReader::GetMaterialInstanceParams(std::string_view a) {
	FORWARD(GetMaterialInstanceParams, a);
}
IBlueprintReader::StaticMeshInfoResult
AutoBlueprintReader::GetStaticMeshInfo(std::string_view a) {
	FORWARD(GetStaticMeshInfo, a);
}
IBlueprintReader::UmgEditorStateResult
AutoBlueprintReader::GetUmgEditorState(std::string_view a) {
	FORWARD(GetUmgEditorState, a);
}
IBlueprintReader::MaterialEditorStateResult
AutoBlueprintReader::GetMaterialEditorState(std::string_view a) {
	FORWARD(GetMaterialEditorState, a);
}
IBlueprintReader::MeshPreviewStateResult
AutoBlueprintReader::GetMeshPreviewState(std::string_view a) {
	FORWARD(GetMeshPreviewState, a);
}
IBlueprintReader::CinematicCameraResult
AutoBlueprintReader::GetCinematicCamera() {
	FORWARD(GetCinematicCamera);
}
IBlueprintReader::SequencerStateResult
AutoBlueprintReader::GetSequencerState(std::string_view a) {
	FORWARD(GetSequencerState, a);
}
IBlueprintReader::AnimEditorStateResult
AutoBlueprintReader::GetAnimEditorState(std::string_view a) {
	FORWARD(GetAnimEditorState, a);
}
IBlueprintReader::NiagaraModuleSelectionResult
AutoBlueprintReader::GetNiagaraModuleSelection(std::string_view a) {
	FORWARD(GetNiagaraModuleSelection, a);
}
IBlueprintReader::CurveEditorSelectionResult
AutoBlueprintReader::GetCurveEditorSelection(std::string_view a) {
	FORWARD(GetCurveEditorSelection, a);
}
IBlueprintReader::BufferVizModeResult AutoBlueprintReader::GetBufferVisualizationMode() {
	FORWARD(GetBufferVisualizationMode);
}
IBlueprintReader::GizmoStateResult AutoBlueprintReader::GetGizmoState() {
	FORWARD(GetGizmoState);
}
IBlueprintReader::ViewportRealtimeResult AutoBlueprintReader::GetViewportRealtime() {
	FORWARD(GetViewportRealtime);
}
IBlueprintReader::ViewportCameraSettingsResult AutoBlueprintReader::GetViewportCameraSettings() {
	FORWARD(GetViewportCameraSettings);
}
IBlueprintReader::SnappingSettingsResult AutoBlueprintReader::GetSnappingSettings() {
	FORWARD(GetSnappingSettings);
}
IBlueprintReader::ActiveViewportResult AutoBlueprintReader::GetActiveViewport() {
	FORWARD(GetActiveViewport);
}
IBlueprintReader::HiddenActorsResult AutoBlueprintReader::GetHiddenActors() {
	FORWARD(GetHiddenActors);
}
IBlueprintReader::VisibleActorsResult
AutoBlueprintReader::GetVisibleActors(std::string_view f, double d) {
	FORWARD(GetVisibleActors, f, d);
}
IBlueprintReader::SetViewModeResult
AutoBlueprintReader::SetViewMode(std::string_view m) {
	FORWARD(SetViewMode, m);
}
IBlueprintReader::SetGizmoModeResult
AutoBlueprintReader::SetGizmoMode(std::string_view m) {
	FORWARD(SetGizmoMode, m);
}
IBlueprintReader::SetViewportRealtimeResult
AutoBlueprintReader::SetViewportRealtime(bool e) {
	FORWARD(SetViewportRealtime, e);
}
IBlueprintReader::SetActorVisibilityResult
AutoBlueprintReader::SetActorVisibility(std::string_view n, bool v) {
	FORWARD(SetActorVisibility, n, v);
}
IBlueprintReader::HiddenLayersResult AutoBlueprintReader::GetHiddenLayers() {
	FORWARD(GetHiddenLayers);
}
IBlueprintReader::SetLayerVisibilityResult
AutoBlueprintReader::SetLayerVisibility(std::string_view l, bool v) {
	FORWARD(SetLayerVisibility, l, v);
}
IBlueprintReader::CameraBookmarksResult AutoBlueprintReader::GetCameraBookmarks() {
	FORWARD(GetCameraBookmarks);
}
IBlueprintReader::GotoBookmarkResult AutoBlueprintReader::GotoCameraBookmark(int s) {
	FORWARD(GotoCameraBookmark, s);
}
IBlueprintReader::HoverTargetResult AutoBlueprintReader::GetHoverTarget() {
	FORWARD(GetHoverTarget);
}
IBlueprintReader::IsolateModeResult AutoBlueprintReader::GetIsolateMode() {
	FORWARD(GetIsolateMode);
}
IBlueprintReader::AsyncCompileStateResult AutoBlueprintReader::GetAsyncCompileState() {
	FORWARD(GetAsyncCompileState);
}
IBlueprintReader::ShaderCompileStateResult AutoBlueprintReader::GetShaderCompileState() {
	FORWARD(GetShaderCompileState);
}
IBlueprintReader::CurrentLevelResult AutoBlueprintReader::GetCurrentLevel() {
	FORWARD(GetCurrentLevel);
}
IBlueprintReader::LoadedLevelsResult AutoBlueprintReader::ListLoadedLevels() {
	FORWARD(ListLoadedLevels);
}
IBlueprintReader::SourceControlProviderResult AutoBlueprintReader::GetSourceControlProvider() {
	FORWARD(GetSourceControlProvider);
}
IBlueprintReader::AssetRegistryStateResult AutoBlueprintReader::GetAssetRegistryState() {
	FORWARD(GetAssetRegistryState);
}
IBlueprintReader::DataLayerStatesResult AutoBlueprintReader::GetDataLayerStates() {
	FORWARD(GetDataLayerStates);
}
IBlueprintReader::AutosaveStatusResult AutoBlueprintReader::GetAutosaveStatus() {
	FORWARD(GetAutosaveStatus);
}
IBlueprintReader::RecoveryStateResult AutoBlueprintReader::GetRecoveryState() {
	FORWARD(GetRecoveryState);
}
IBlueprintReader::SourceControlStatusResult
AutoBlueprintReader::GetSourceControlStatus(std::string_view p) {
	FORWARD(GetSourceControlStatus, p);
}
IBlueprintReader::FileLockStatusResult
AutoBlueprintReader::GetFileLockStatus(std::string_view p) {
	FORWARD(GetFileLockStatus, p);
}
IBlueprintReader::ActiveCultureResult AutoBlueprintReader::GetActiveCulture() {
	FORWARD(GetActiveCulture);
}
IBlueprintReader::EditorThemeResult AutoBlueprintReader::GetEditorTheme() {
	FORWARD(GetEditorTheme);
}
IBlueprintReader::MonitorInfoResult AutoBlueprintReader::GetMonitors() {
	FORWARD(GetMonitors);
}
IBlueprintReader::LiveCodingStateResult AutoBlueprintReader::GetLiveCodingState() {
	FORWARD(GetLiveCodingState);
}
IBlueprintReader::GameFeatureActionResult
AutoBlueprintReader::ActivateGameFeature(std::string_view p) {
	FORWARD(ActivateGameFeature, p);
}
IBlueprintReader::GameFeatureActionResult
AutoBlueprintReader::DeactivateGameFeature(std::string_view p) {
	FORWARD(DeactivateGameFeature, p);
}
IBlueprintReader::RecentAssetsResult AutoBlueprintReader::GetRecentlyOpenedAssets() {
	FORWARD(GetRecentlyOpenedAssets);
}
IBlueprintReader::DebugInstanceResult
AutoBlueprintReader::GetDebugInstance(std::string_view p) {
	FORWARD(GetDebugInstance, p);
}
IBlueprintReader::BreakpointsResult
AutoBlueprintReader::GetBlueprintBreakpoints(std::string_view p) {
	FORWARD(GetBlueprintBreakpoints, p);
}
IBlueprintReader::WatchedPinsResult
AutoBlueprintReader::GetWatchedPins(std::string_view p) {
	FORWARD(GetWatchedPins, p);
}
IBlueprintReader::ActiveStatsResult AutoBlueprintReader::GetActiveStats() {
	FORWARD(GetActiveStats);
}
IBlueprintReader::SetPluginEnabledResult
AutoBlueprintReader::SetPluginEnabled(std::string_view n, bool e) {
	FORWARD(SetPluginEnabled, n, e);
}
IBlueprintReader::StreamingSourcesResult AutoBlueprintReader::GetStreamingSources() {
	FORWARD(GetStreamingSources);
}
IBlueprintReader::RecentSavedPackagesResult AutoBlueprintReader::GetRecentlySavedPackages() {
	FORWARD(GetRecentlySavedPackages);
}
IBlueprintReader::ProjectSettingsResult AutoBlueprintReader::ListProjectSettings() {
	FORWARD(ListProjectSettings);
}
IBlueprintReader::ProjectSettingValuesResult
AutoBlueprintReader::GetProjectSettingValues(std::string_view p) {
	FORWARD(GetProjectSettingValues, p);
}
IBlueprintReader::SetProjectSettingResult
AutoBlueprintReader::SetProjectSetting(std::string_view c, std::string_view p, std::string_view v) {
	FORWARD(SetProjectSetting, c, p, v);
}
IBlueprintReader::AutomationTestsResult AutoBlueprintReader::ListAutomationTests() {
	FORWARD(ListAutomationTests);
}
IBlueprintReader::EditorEventsResult AutoBlueprintReader::GetEditorEvents() {
	FORWARD(GetEditorEvents);
}
IBlueprintReader::CookTargetResult AutoBlueprintReader::GetActiveCookTarget() {
	FORWARD(GetActiveCookTarget);
}
IBlueprintReader::WorkspaceLayoutResult AutoBlueprintReader::GetWorkspaceLayout() {
	FORWARD(GetWorkspaceLayout);
}
IBlueprintReader::TraceStateResult AutoBlueprintReader::GetTraceState() {
	FORWARD(GetTraceState);
}
IBlueprintReader::UiStateStubResult AutoBlueprintReader::GetUiStateStub(std::string_view feature) {
	FORWARD(GetUiStateStub, feature);
}

// ----- Full editor-action + per-asset-type op surface ---------------------
// These forwarders were missing; without them the default "auto" backend
// hit IBlueprintReader's throwing defaults for every material / data-table /
// widget / behavior-tree / actor / console / profiling / cook / config op.

IBlueprintReader::AddAnimStateResult AutoBlueprintReader::AddAnimState(std::string_view assetPath, std::string_view stateMachine, std::string_view stateName) {
	FORWARD(AddAnimState, assetPath, stateMachine, stateName);
}
IBlueprintReader::AddBTNodeResult AutoBlueprintReader::AddBTNode(std::string_view assetPath, std::string_view parentNodeId, std::string_view nodeKind, std::string_view nodeClass) {
	FORWARD(AddBTNode, assetPath, parentNodeId, nodeKind, nodeClass);
}
IBlueprintReader::AddComponentResult AutoBlueprintReader::AddComponent(std::string_view assetPath, std::string_view name, std::string_view componentClass, std::string_view parentName, std::string_view socket) {
	FORWARD(AddComponent, assetPath, name, componentClass, parentName, socket);
}
IBlueprintReader::AddDataRowResult AutoBlueprintReader::AddDataRow(std::string_view assetPath, std::string_view rowName, const nlohmann::json& values, bool overwrite) {
	FORWARD(AddDataRow, assetPath, rowName, values, overwrite);
}
IBlueprintReader::AddGameplayTagResult AutoBlueprintReader::AddGameplayTag(std::string_view tagName, std::string_view comment) {
	FORWARD(AddGameplayTag, tagName, comment);
}
IBlueprintReader::AddMaterialExpressionResult AutoBlueprintReader::AddMaterialExpression(std::string_view assetPath, std::string_view expressionClass, int x, int y) {
	FORWARD(AddMaterialExpression, assetPath, expressionClass, x, y);
}
IBlueprintReader::AddSequenceTrackResult AutoBlueprintReader::AddSequenceTrack(std::string_view assetPath, std::string_view trackClass, std::string_view trackName) {
	FORWARD(AddSequenceTrack, assetPath, trackClass, trackName);
}
IBlueprintReader::AddStateTreeStateResult AutoBlueprintReader::AddStateTreeState(std::string_view assetPath, std::string_view parentStateId, std::string_view name) {
	FORWARD(AddStateTreeState, assetPath, parentStateId, name);
}
IBlueprintReader::AddWidgetResult AutoBlueprintReader::AddWidget(std::string_view assetPath, std::string_view parentName, std::string_view widgetClass, std::string_view name) {
	FORWARD(AddWidget, assetPath, parentName, widgetClass, name);
}
IBlueprintReader::AttachComponentResult AutoBlueprintReader::AttachComponent(std::string_view assetPath, std::string_view name, std::string_view newParentName, std::string_view socket) {
	FORWARD(AttachComponent, assetPath, name, newParentName, socket);
}
IBlueprintReader::BindWidgetEventResult AutoBlueprintReader::BindWidgetEvent(std::string_view assetPath, std::string_view widgetName, std::string_view eventName, std::string_view handlerFunction) {
	FORWARD(BindWidgetEvent, assetPath, widgetName, eventName, handlerFunction);
}
IBlueprintReader::BuildLightingResult AutoBlueprintReader::BuildLighting(std::string_view quality) {
	FORWARD(BuildLighting, quality);
}
IBlueprintReader::CompileAnimBlueprintResult AutoBlueprintReader::CompileAnimBlueprint(std::string_view assetPath) {
	FORWARD(CompileAnimBlueprint, assetPath);
}
IBlueprintReader::CompileBehaviorTreeResult AutoBlueprintReader::CompileBehaviorTree(std::string_view assetPath) {
	FORWARD(CompileBehaviorTree, assetPath);
}
IBlueprintReader::CompileMaterialResult AutoBlueprintReader::CompileMaterial(std::string_view assetPath) {
	FORWARD(CompileMaterial, assetPath);
}
IBlueprintReader::CompileStateTreeResult AutoBlueprintReader::CompileStateTree(std::string_view assetPath) {
	FORWARD(CompileStateTree, assetPath);
}
IBlueprintReader::CompileWidgetBlueprintResult AutoBlueprintReader::CompileWidgetBlueprint(std::string_view assetPath) {
	FORWARD(CompileWidgetBlueprint, assetPath);
}
IBlueprintReader::ConnectMaterialResult AutoBlueprintReader::ConnectMaterialExpressions(std::string_view assetPath, std::string_view fromNodeId, std::string_view fromPin, std::string_view toNodeId, std::string_view toPin) {
	FORWARD(ConnectMaterialExpressions, assetPath, fromNodeId, fromPin, toNodeId, toPin);
}
IBlueprintReader::ConsoleCommandResult AutoBlueprintReader::ConsoleCommand(std::string_view command) {
	FORWARD(ConsoleCommand, command);
}
IBlueprintReader::CookResult AutoBlueprintReader::CookContent(std::string_view platform) {
	FORWARD(CookContent, platform);
}
IBlueprintReader::CreateDataAssetResult AutoBlueprintReader::CreateDataAsset(std::string_view assetPath, std::string_view className) {
	FORWARD(CreateDataAsset, assetPath, className);
}
IBlueprintReader::CreateFolderResult AutoBlueprintReader::CreateFolder(std::string_view folderPath) {
	FORWARD(CreateFolder, folderPath);
}
IBlueprintReader::CreateNiagaraSystemResult AutoBlueprintReader::CreateNiagaraSystem(std::string_view assetPath) {
	FORWARD(CreateNiagaraSystem, assetPath);
}
IBlueprintReader::DeleteActorResult AutoBlueprintReader::DeleteActor(std::string_view actorName) {
	FORWARD(DeleteActor, actorName);
}
IBlueprintReader::DeleteAssetResult AutoBlueprintReader::DeleteAsset(std::string_view assetPath, bool force) {
	FORWARD(DeleteAsset, assetPath, force);
}
IBlueprintReader::FindClassResult AutoBlueprintReader::FindClass(std::string_view query) {
	FORWARD(FindClass, query);
}
IBlueprintReader::FocusActorResult AutoBlueprintReader::FocusActor(std::string_view actorName) {
	FORWARD(FocusActor, actorName);
}
IBlueprintReader::CVarValue AutoBlueprintReader::GetCVar(std::string_view name) {
	FORWARD(GetCVar, name);
}
IBlueprintReader::AssetGraphResult AutoBlueprintReader::GetDependencies(std::string_view assetPath) {
	FORWARD(GetDependencies, assetPath);
}
BPRJson AutoBlueprintReader::GetEditorState() {
	FORWARD(GetEditorState);
}
IBlueprintReader::AssetGraphResult AutoBlueprintReader::GetReferencers(std::string_view assetPath) {
	FORWARD(GetReferencers, assetPath);
}
IBlueprintReader::SelectionResult AutoBlueprintReader::GetSelectedActors() {
	FORWARD(GetSelectedActors);
}
IBlueprintReader::StatGroupResult AutoBlueprintReader::GetStats(std::string_view group) {
	FORWARD(GetStats, group);
}
IBlueprintReader::ClassInfo AutoBlueprintReader::IntrospectClass(std::string_view className) {
	FORWARD(IntrospectClass, className);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListAnimBlueprints(std::string_view path) {
	FORWARD(ListAnimBlueprints, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListBehaviorTrees(std::string_view path) {
	FORWARD(ListBehaviorTrees, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListDataAssets(std::string_view path) {
	FORWARD(ListDataAssets, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListDataTables(std::string_view path) {
	FORWARD(ListDataTables, path);
}
std::vector<IBlueprintReader::ClassFunctionInfo> AutoBlueprintReader::ListFunctions(std::string_view className) {
	FORWARD(ListFunctions, className);
}
IBlueprintReader::GameplayTagListResult AutoBlueprintReader::ListGameplayTags(std::string_view filter) {
	FORWARD(ListGameplayTags, filter);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListLevelSequences(std::string_view path) {
	FORWARD(ListLevelSequences, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListMaterials(std::string_view path) {
	FORWARD(ListMaterials, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListNiagaraSystems(std::string_view path) {
	FORWARD(ListNiagaraSystems, path);
}
std::vector<BPAssetSummary> AutoBlueprintReader::ListStateTrees(std::string_view path) {
	FORWARD(ListStateTrees, path);
}
IBlueprintReader::LiveCodingResult AutoBlueprintReader::LiveCodingCompile() {
	FORWARD(LiveCodingCompile);
}
IBlueprintReader::MoveAssetResult AutoBlueprintReader::MoveAsset(std::string_view sourcePath, std::string_view destPath) {
	FORWARD(MoveAsset, sourcePath, destPath);
}
IBlueprintReader::CookResult AutoBlueprintReader::PackageProject(std::string_view platform, std::string_view outputDir) {
	FORWARD(PackageProject, platform, outputDir);
}
IBlueprintReader::PieResult AutoBlueprintReader::PieStart(std::string_view mode) {
	FORWARD(PieStart, mode);
}
IBlueprintReader::PieResult AutoBlueprintReader::PieStop() {
	FORWARD(PieStop);
}
IBlueprintReader::AbilitySetInfo AutoBlueprintReader::ReadAbilitySet(std::string_view assetPath) {
	FORWARD(ReadAbilitySet, assetPath);
}
IBlueprintReader::AnimBlueprintInfo AutoBlueprintReader::ReadAnimBlueprint(std::string_view assetPath) {
	FORWARD(ReadAnimBlueprint, assetPath);
}
IBlueprintReader::BehaviorTreeInfo AutoBlueprintReader::ReadBehaviorTree(std::string_view assetPath) {
	FORWARD(ReadBehaviorTree, assetPath);
}
IBlueprintReader::ConfigReadResult AutoBlueprintReader::ReadConfigValue(std::string_view section, std::string_view key, std::string_view file) {
	FORWARD(ReadConfigValue, section, key, file);
}
IBlueprintReader::DataAssetInfo AutoBlueprintReader::ReadDataAsset(std::string_view assetPath) {
	FORWARD(ReadDataAsset, assetPath);
}
IBlueprintReader::DataTableInfo AutoBlueprintReader::ReadDataTable(std::string_view assetPath) {
	FORWARD(ReadDataTable, assetPath);
}
IBlueprintReader::LevelSequenceInfo AutoBlueprintReader::ReadLevelSequence(std::string_view assetPath) {
	FORWARD(ReadLevelSequence, assetPath);
}
IBlueprintReader::MaterialInfo AutoBlueprintReader::ReadMaterial(std::string_view assetPath) {
	FORWARD(ReadMaterial, assetPath);
}
IBlueprintReader::NiagaraSystemInfo AutoBlueprintReader::ReadNiagaraSystem(std::string_view assetPath) {
	FORWARD(ReadNiagaraSystem, assetPath);
}
IBlueprintReader::OutputLogResult AutoBlueprintReader::ReadOutputLog(int limit, std::string_view minSeverity) {
	FORWARD(ReadOutputLog, limit, minSeverity);
}
IBlueprintReader::StateTreeInfo AutoBlueprintReader::ReadStateTree(std::string_view assetPath) {
	FORWARD(ReadStateTree, assetPath);
}
IBlueprintReader::WidgetBlueprintInfo AutoBlueprintReader::ReadWidgetBlueprint(std::string_view assetPath) {
	FORWARD(ReadWidgetBlueprint, assetPath);
}
IBlueprintReader::RemoveComponentResult AutoBlueprintReader::RemoveComponent(std::string_view assetPath, std::string_view name) {
	FORWARD(RemoveComponent, assetPath, name);
}
IBlueprintReader::AutomationRunResult AutoBlueprintReader::RunAutomationTests(std::string_view pattern) {
	FORWARD(RunAutomationTests, pattern);
}
IBlueprintReader::SaveAllResult AutoBlueprintReader::SaveAll(bool dirtyOnly) {
	FORWARD(SaveAll, dirtyOnly);
}
void AutoBlueprintReader::SetActorTransform(std::string_view actorName, double locX, double locY, double locZ, double rotPitch, double rotYaw, double rotRoll, double scaleX, double scaleY, double scaleZ) {
	FORWARD_VOID(SetActorTransform, actorName, locX, locY, locZ, rotPitch, rotYaw, rotRoll, scaleX, scaleY, scaleZ);
}
IBlueprintReader::SetBTNodePropertyResult AutoBlueprintReader::SetBTNodeProperty(std::string_view assetPath, std::string_view nodeId, std::string_view propertyName, std::string_view value) {
	FORWARD(SetBTNodeProperty, assetPath, nodeId, propertyName, value);
}
IBlueprintReader::SetCameraResult AutoBlueprintReader::SetCameraTransform(double lx, double ly, double lz, double rp, double ry, double rr) {
	FORWARD(SetCameraTransform, lx, ly, lz, rp, ry, rr);
}
IBlueprintReader::SetComponentPropertyResult AutoBlueprintReader::SetComponentProperty(std::string_view assetPath, std::string_view componentName, std::string_view propertyName, std::string_view value) {
	FORWARD(SetComponentProperty, assetPath, componentName, propertyName, value);
}
IBlueprintReader::ConfigWriteResult AutoBlueprintReader::SetConfigValue(std::string_view section, std::string_view key, std::string_view value, std::string_view file) {
	FORWARD(SetConfigValue, section, key, value, file);
}
IBlueprintReader::CVarValue AutoBlueprintReader::SetCVar(std::string_view name, std::string_view value) {
	FORWARD(SetCVar, name, value);
}
IBlueprintReader::SetDataAssetPropertyResult AutoBlueprintReader::SetDataAssetProperty(std::string_view assetPath, std::string_view propertyName, std::string_view value) {
	FORWARD(SetDataAssetProperty, assetPath, propertyName, value);
}
IBlueprintReader::SetDataRowValueResult AutoBlueprintReader::SetDataRowValue(std::string_view assetPath, std::string_view rowName, std::string_view fieldName, std::string_view value) {
	FORWARD(SetDataRowValue, assetPath, rowName, fieldName, value);
}
IBlueprintReader::SetMIParameterResult AutoBlueprintReader::SetMaterialInstanceParameter(std::string_view assetPath, std::string_view parameterName, std::string_view paramType, std::string_view value) {
	FORWARD(SetMaterialInstanceParameter, assetPath, parameterName, paramType, value);
}
IBlueprintReader::SetMaterialParameterResult AutoBlueprintReader::SetMaterialParameter(std::string_view assetPath, std::string_view parameterName, std::string_view value) {
	FORWARD(SetMaterialParameter, assetPath, parameterName, value);
}
IBlueprintReader::SetNiagaraParameterResult AutoBlueprintReader::SetNiagaraParameter(std::string_view assetPath, std::string_view parameterName, std::string_view value) {
	FORWARD(SetNiagaraParameter, assetPath, parameterName, value);
}
IBlueprintReader::SelectionResult AutoBlueprintReader::SetSelection(const std::vector<std::string>& actorNames, bool replace) {
	FORWARD(SetSelection, actorNames, replace);
}
IBlueprintReader::SetSequencePlaybackRangeResult AutoBlueprintReader::SetSequencePlaybackRange(std::string_view assetPath, double startSeconds, double endSeconds) {
	FORWARD(SetSequencePlaybackRange, assetPath, startSeconds, endSeconds);
}
IBlueprintReader::SetShowFlagResult AutoBlueprintReader::SetShowFlag(std::string_view flagName, bool enabled) {
	FORWARD(SetShowFlag, flagName, enabled);
}
IBlueprintReader::SetStateTreeTransitionResult AutoBlueprintReader::SetStateTreeTransition(std::string_view assetPath, std::string_view fromStateId, std::string_view toStateId, std::string_view trigger) {
	FORWARD(SetStateTreeTransition, assetPath, fromStateId, toStateId, trigger);
}
IBlueprintReader::SetWidgetPropertyResult AutoBlueprintReader::SetWidgetProperty(std::string_view assetPath, std::string_view widgetName, std::string_view propertyName, std::string_view value) {
	FORWARD(SetWidgetProperty, assetPath, widgetName, propertyName, value);
}
IBlueprintReader::SpawnActorResult AutoBlueprintReader::SpawnActor(std::string_view classPath, double locX, double locY, double locZ, double rotPitch, double rotYaw, double rotRoll, double scaleX, double scaleY, double scaleZ) {
	FORWARD(SpawnActor, classPath, locX, locY, locZ, rotPitch, rotYaw, rotRoll, scaleX, scaleY, scaleZ);
}
IBlueprintReader::StartProfileResult AutoBlueprintReader::StartProfile(std::string_view mode) {
	FORWARD(StartProfile, mode);
}
IBlueprintReader::StopProfileResult AutoBlueprintReader::StopProfile() {
	FORWARD(StopProfile);
}
IBlueprintReader::ScreenshotResult AutoBlueprintReader::TakeScreenshot(std::string_view destPath, int width, int height) {
	FORWARD(TakeScreenshot, destPath, width, height);
}
IBlueprintReader::ViewportScreenshotResult AutoBlueprintReader::TakeViewportScreenshot(std::string_view destPath) {
	FORWARD(TakeViewportScreenshot, destPath);
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
