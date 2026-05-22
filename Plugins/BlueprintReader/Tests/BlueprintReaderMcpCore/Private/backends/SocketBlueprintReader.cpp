#include "backends/SocketBlueprintReader.h"

#include <fmt/core.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif    // WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "Ws2_32.lib")
	using SocketType = SOCKET;
	static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else    // defined(_WIN32)
	using SocketType = int;
	static constexpr SocketType kInvalidSocket = -1;
#endif    // defined(_WIN32)

namespace bpr::backends {

namespace socket_blueprint_reader_detail {

// Translation table for the per-op `code` field the editor returns on
// the wire (see Plugins/BlueprintReader/Source/BlueprintReaderEditor/
// Private/BlueprintReaderCommandlet.cpp — every Run*Op handler uses
// the same code vocabulary). The numeric codes are stable across ops;
// the *meaning* varies slightly per op (code 4 is "BP not found" for
// some, "graph/node/pin not found" for others), so we surface a name
// + general description rather than guessing one specific cause.
//
// Without this translation, the agent gets `live op ... returned
// code=4` and has to guess. With it, they get `NotFound (code=4):
// asset, graph, node, pin, ...` which is enough to know what kind of
// thing went wrong and what to check.
struct ErrorCodeInfo {
	const char* name;
	const char* description;
};
const ErrorCodeInfo* LookupErrorCode(int code) {
	switch (code) {
		case 1: { static const ErrorCodeInfo i = {
			"BadRequest",
			"the editor op rejected the arguments — a required field "
			"(-Asset, -Graph, etc.) was missing or malformed"
		}; return &i; }
		case 2: { static const ErrorCodeInfo i = {
			"BlueprintNotFound",
			"LoadObject<UBlueprint> returned null — the path didn't "
			"resolve, the asset isn't loaded, or it isn't a Blueprint"
		}; return &i; }
		case 3: { static const ErrorCodeInfo i = {
			"WriteFailed",
			"the editor couldn't write the response JSON to disk"
		}; return &i; }
		case 4: { static const ErrorCodeInfo i = {
			"NotFound",
			"a referenced sub-resource wasn't located — asset, graph, "
			"node, pin, class, or (for typed-BP read ops) a required "
			"attribute like WidgetTree was missing"
		}; return &i; }
		case 5: { static const ErrorCodeInfo i = {
			"CompileSaveFailed",
			"the BP was modified successfully but the recompile or "
			"save step failed — check the engine log for the compile "
			"errors that surfaced"
		}; return &i; }
		default: return nullptr;
	}
}

// Pull `-Asset=` (or any single -Key=value flag) out of the op args
// for inclusion in error messages — gives the agent the specific
// path that failed without their having to recall the call site.
std::string ExtractFlag(const std::vector<std::string>& args, std::string_view flag) {
	const std::string prefix(flag);
	for (const auto& a : args) {
		if (a.size() > prefix.size() && a.compare(0, prefix.size(), prefix) == 0) {
			return a.substr(prefix.size());
		}
	}
	return {};
}

// On Windows, WSAStartup is required before any socket call. We refcount
// across SocketBlueprintReader instances so the startup/cleanup pair is
// balanced even if multiple readers exist (rare, but cheap to be right).
#if defined(_WIN32)
struct WsaScope {
	WsaScope() {
		WSADATA d;
		if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
			throw BlueprintReaderError("WSAStartup failed");
		}
	}
	~WsaScope() { WSACleanup(); }
};
WsaScope& Wsa() {
	static WsaScope s;
	return s;
}
#endif    // defined(_WIN32)

// Send all bytes or throw. Wraps the loop pattern around partial sends.
void SendAll(SocketType s, const char* data, std::size_t len) {
	std::size_t sent = 0;
	while (sent < len) {
		int n = ::send(s, data + sent,
					   static_cast<int>(std::min<std::size_t>(len - sent, 1 << 20)),
					   0);
		if (n <= 0) {
			throw BlueprintReaderError("SocketBlueprintReader: socket write failed");
		}
		sent += static_cast<std::size_t>(n);
	}
}

// Read a newline-terminated frame. Buffers extra bytes (the next frame
// may have arrived in the same recv) into `pending` for later reads.
std::string RecvLine(SocketType s, std::string& pending) {
	while (true) {
		auto nl = pending.find('\n');
		if (nl != std::string::npos) {
			std::string line = pending.substr(0, nl);
			pending.erase(0, nl + 1);
			// Trim trailing \r if present (defensive against CRLF servers).
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.pop_back();
			}
			return line;
		}
		char buf[4096];
		int n = ::recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			throw BlueprintReaderError(
				"SocketBlueprintReader: connection closed before frame complete");
		}
		pending.append(buf, static_cast<std::size_t>(n));
	}
}

// Per-instance read buffer. Wrap in a struct so RecvLine's `pending` ref
// has somewhere persistent to live across calls without leaking buffer
// state into the public header. The map is thread_local because UE
// commandlet daemon clients run from a single MCP-server thread per
// reader — but Disconnect() must erase the entry, or a reused socket
// fd (after editor restart) would pick up stale `pending` bytes from
// the dead session and misframe the next response.
struct PendingBuf {
	std::string b;
};
using BufMap = std::map<SocketType, PendingBuf>;
BufMap& BufsForThisThread() {
	thread_local BufMap bufs;
	return bufs;
}
PendingBuf& BufFor(SocketType s) {
	return BufsForThisThread()[s];
}
void EraseBufFor(SocketType s) {
	BufsForThisThread().erase(s);
}

// Close a socket portably. Used by Disconnect and ScopedSocket.
inline void CloseSocketCompat(SocketType s) {
	if (s == kInvalidSocket)
	{
		return;
	}
#if defined(_WIN32)
	closesocket(s);
#else    // defined(_WIN32)
	close(s);
#endif    // defined(_WIN32)
}

// RAII guard for a socket fd. On scope exit the socket is closed unless
// release() was called. Removes the seven `#if defined(_WIN32)` cleanup
// blocks TryConnectAndHandshake otherwise needs on every failure branch.
struct ScopedSocket {
	SocketType s = kInvalidSocket;
	explicit ScopedSocket(SocketType in) : s(in) {}
	~ScopedSocket() { CloseSocketCompat(s); }
	ScopedSocket(const ScopedSocket&) = delete;
	ScopedSocket& operator=(const ScopedSocket&) = delete;
	SocketType release() { SocketType r = s; s = kInvalidSocket; return r; }
	explicit operator bool() const { return s != kInvalidSocket; }
};

}    // namespace socket_blueprint_reader_detail
using namespace socket_blueprint_reader_detail;

SocketBlueprintReader::SocketBlueprintReader(Config cfg) : cfg_(std::move(cfg)) {
	if (cfg_.port <= 0) {
		throw BlueprintReaderError(
			"SocketBlueprintReader: BP_READER_LIVE_PORT must be set to a valid port");
	}
	if (cfg_.token.empty()) {
		throw BlueprintReaderError(
			"SocketBlueprintReader: BP_READER_LIVE_TOKEN must be set "
			"(also required on the editor side; the values must match)");
	}
#if defined(_WIN32)
	(void)Wsa();  // first construction triggers WSAStartup
#endif    // defined(_WIN32)
	// Connect lazily on first RunOp — keeps construction cheap and
	// non-throwing when the editor isn't running yet.
}

SocketBlueprintReader::~SocketBlueprintReader() {
	Disconnect();
}

void SocketBlueprintReader::Disconnect() {
	if (socket_ != static_cast<intptr_t>(kInvalidSocket)) {
		const SocketType s = static_cast<SocketType>(socket_);
		CloseSocketCompat(s);
		// Drop the per-socket read buffer so a future reconnect that
		// happens to get the same fd back from the kernel doesn't pick
		// up leftover `pending` bytes from this dead session.
		EraseBufFor(s);
		socket_ = static_cast<intptr_t>(kInvalidSocket);
	}
	handshakeOk_ = false;
}

// Try to read `<Project>/Saved/bp-reader-live.json` and refresh cfg_'s
// host/port/token in place. Returns true when the file existed, parsed,
// and produced a different (host,port,token) than the current cfg_ —
// i.e. when the editor restarted and is now listening on a different
// port. Returns false on "no handshake file" / "same as before" / "any
// parse failure" — callers treat that as "nothing to retry with."
bool SocketBlueprintReader::RefreshFromHandshakeFile() {
	if (cfg_.handshakeFilePath.empty())
	{
		return false;
	}
	std::error_code ec;
	if (!std::filesystem::exists(cfg_.handshakeFilePath, ec))
	{
		return false;
	}
	std::ifstream f(cfg_.handshakeFilePath);
	if (!f)
	{
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	nlohmann::json j;
	try { j = nlohmann::json::parse(ss.str()); }
	catch (...) { return false; }
	std::string newHost  = j.value("host",  std::string("127.0.0.1"));
	int         newPort  = j.value("port",  0);
	std::string newToken = j.value("token", std::string());
	if (newPort <= 0 || newToken.empty())
	{
		return false;
	}
	if (newHost == cfg_.host && newPort == cfg_.port && newToken == cfg_.token) {
		return false;  // identical to current — nothing to retry with
	}
	cfg_.host  = std::move(newHost);
	cfg_.port  = newPort;
	cfg_.token = std::move(newToken);
	return true;
}

// Open a TCP connection to host:port. Returns kInvalidSocket on failure.
// CloseSocketCompat + ScopedSocket helpers live in the anonymous
// namespace above so Disconnect (declared earlier) can reach them.
namespace socket_blueprint_reader_detail2 {
SocketType ConnectOnce(const std::string& host, int port) {
	ScopedSocket sock(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	if (!sock)
	{
		return kInvalidSocket;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
	if (::connect(sock.s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		return kInvalidSocket;  // ScopedSocket closes on scope exit
	}
	return sock.release();
}
}    // namespace socket_blueprint_reader_detail2
using namespace socket_blueprint_reader_detail2;

// One connect + handshake pass. Leaves socket_ + handshakeOk_
// untouched on failure (caller decides whether to retry). The
// retryWorthwhile hint distinguishes "the editor probably restarted,
// re-read the handshake file" from "something is structurally wrong"
// — see the AttemptResult declaration in SocketBlueprintReader.h.
//
// All early-exit paths rely on ScopedSocket's destructor to close the
// fd; we only `release()` once the handshake fully succeeds and the
// caller takes ownership.
SocketBlueprintReader::AttemptResult SocketBlueprintReader::TryConnectAndHandshake() {
	ScopedSocket sock(ConnectOnce(cfg_.host, cfg_.port));
	if (!sock) {
		return {false, /*retryWorthwhile=*/true, fmt::format(
			"connect to {}:{} failed", cfg_.host, cfg_.port)};
	}

	auto& buf = BufFor(sock.s).b;
	buf.clear();

	std::string hello;
	try {
		hello = RecvLine(sock.s, buf);
	} catch (const std::exception& e) {
		// Protocol-level failure on a fresh connect typically means the
		// server we connected to isn't the editor — a refresh probably
		// won't help, so don't burn a retry on it.
		return {false, /*retryWorthwhile=*/false, fmt::format(
			"reading hello frame failed: {}", e.what())};
	}
	auto helloJson = nlohmann::json::parse(hello, nullptr, /*allow_exceptions=*/false);
	if (!helloJson.is_object() || helloJson.value("type", "") != "hello") {
		return {false, false, fmt::format(
			"expected hello frame, got: {}", hello)};
	}

	nlohmann::json authMsg = { {"type", "auth"}, {"token", cfg_.token} };
	std::string authLine = authMsg.dump() + "\n";
	try {
		SendAll(sock.s, authLine.data(), authLine.size());
	} catch (const std::exception& e) {
		return {false, true, fmt::format("auth send failed: {}", e.what())};
	}

	std::string authResp;
	try {
		authResp = RecvLine(sock.s, buf);
	} catch (const std::exception& e) {
		return {false, true, fmt::format("auth response read failed: {}", e.what())};
	}
	auto authJson = nlohmann::json::parse(authResp, nullptr, false);
	std::string respType = authJson.is_object() ? authJson.value("type", "") : "";
	if (respType != "auth_ok") {
		// Auth failure on stable-port restarts (PR #49) means the token
		// rotated — a handshake refresh fixes it. retryWorthwhile=true.
		return {false, true, fmt::format(
			"auth failed (server response: {})", authResp)};
	}

	// Success — take ownership of the socket from ScopedSocket; the
	// destructor will leave it alone.
	socket_ = static_cast<intptr_t>(sock.release());
	handshakeOk_ = true;
	return {true, false, {}};
}

void SocketBlueprintReader::EnsureConnected() {
	if (handshakeOk_)
	{
		return;
	}

	// Up to two attempts: first with current cfg_, second after re-
	// reading the handshake file if the first failed in a way that
	// a refresh could plausibly fix (connect-refused or auth-failed).
	// Covers BOTH "editor restarted, new port" and "editor restarted,
	// same port but new token" — the latter is the PR #49 stable-port
	// case (issue #9).
	AttemptResult r = TryConnectAndHandshake();
	if (!r.ok && r.retryWorthwhile && RefreshFromHandshakeFile()) {
		r = TryConnectAndHandshake();
	}
	if (!r.ok) {
		throw BlueprintReaderError(fmt::format(
			"SocketBlueprintReader: {} — is the editor running with "
			"BP_READER_LIVE_PORT/TOKEN published in Saved/bp-reader-live.json?",
			r.error));
	}
}

nlohmann::json SocketBlueprintReader::RunOp(const std::vector<std::string>& args) {
	std::lock_guard lock(mu_);
	EnsureConnected();
	SocketType s = static_cast<SocketType>(socket_);

	int id = nextRequestId_++;
	nlohmann::json frame = {
		{"type", "op"},
		{"id", id},
		{"args", args},
	};
	std::string line = frame.dump() + "\n";
	try {
		SendAll(s, line.data(), line.size());
	} catch (...) {
		Disconnect();  // socket is broken; force re-handshake on next call
		throw;
	}

	auto& buf = BufFor(s).b;
	std::string response;
	try {
		response = RecvLine(s, buf);
	} catch (...) {
		Disconnect();
		throw;
	}
	auto j = nlohmann::json::parse(response, nullptr, false);
	if (!j.is_object()) {
		Disconnect();
		throw BlueprintReaderError(
			"SocketBlueprintReader: server response wasn't a JSON object");
	}
	if (j.value("type", "") == "error") {
		throw BlueprintReaderError(fmt::format(
			"live op '{}': {}",
			args.empty() ? "<unknown>" : args[0],
			j.value("error", "unspecified error")));
	}
	int code = j.value("code", -1);
	if (code != 0) {
		const std::string opName = args.empty() ? "<unknown>" : args[0];
		const std::string assetPath = ExtractFlag(args, "-Asset=");
		const auto* info = LookupErrorCode(code);
		const std::string name = info ? info->name : "UnknownCode";
		// Prefer an editor-provided "error" field inside the json body —
		// the editor knows what actually went wrong (e.g. "asset is a
		// Blueprint, not a StateTree") with more context than the static
		// LookupErrorCode description. Fall back to the canned description
		// when the editor didn't include one.
		std::string innerError;
		if (auto jit = j.find("json"); jit != j.end() && jit->is_object()) {
			if (auto eit = jit->find("error"); eit != jit->end() && eit->is_string()) {
				innerError = eit->get<std::string>();
			}
		}
		const std::string description = innerError.empty()
			? (info ? std::string(info->description)
					: std::string("the editor returned a code "
								  "this server doesn't have a translation for — "
								  "see the engine log for context"))
			: innerError;
		std::string msg = fmt::format("{} (code={}): {}", name, code, description);
		if (!assetPath.empty()) {
			msg = fmt::format("{} [asset={}, op={}]", msg, assetPath, opName);
		} else {
			msg = fmt::format("{} [op={}]", msg, opName);
		}
		// Throw AssetNotFound for code 2 specifically (definitively a
		// missing blueprint) so the MCP tool layer can offer "did you
		// mean" candidates from the asset registry. Code 4 is more
		// ambiguous — could be sub-resource — so it stays a generic
		// BlueprintReaderError.
		if (code == 2) {
			throw AssetNotFound(msg);
		}
		throw BlueprintReaderError(msg);
	}
	auto jit = j.find("json");
	if (jit == j.end())
	{
		return nlohmann::json::object();
	}
	return *jit;
}

// ----- read tools --------------------------------------------------------
// Same op-args shape as CommandletBlueprintReader, just routed over TCP.
// Type conversion uses nlohmann::json's automatic deserialization adapters
// in BlueprintReaderTypes.h.

std::vector<BPAssetSummary>
SocketBlueprintReader::ListBlueprints(std::string_view path) {
	std::vector<std::string> args = {"-Op=List"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	return RunOp(args).get<std::vector<BPAssetSummary>>();
}

BPMetadata SocketBlueprintReader::ReadBlueprint(std::string_view assetPath) {
	return RunOp({"-Op=Read", "-Asset=" + std::string(assetPath)}).get<BPMetadata>();
}

BPGraph SocketBlueprintReader::GetGraph(std::string_view assetPath, std::string_view graphName) {
	return RunOp({
		"-Op=Graph",
		"-Asset=" + std::string(assetPath),
		"-Graph=" + std::string(graphName),
	}).get<BPGraph>();
}

BPFunction SocketBlueprintReader::GetFunction(std::string_view assetPath, std::string_view fnName) {
	return RunOp({
		"-Op=Function",
		"-Asset=" + std::string(assetPath),
		"-Function=" + std::string(fnName),
	}).get<BPFunction>();
}

std::vector<BPVariable>
SocketBlueprintReader::ListVariables(std::string_view assetPath) {
	return RunOp({
		"-Op=Variables",
		"-Asset=" + std::string(assetPath),
	}).get<std::vector<BPVariable>>();
}

std::vector<BPComponent>
SocketBlueprintReader::GetComponents(std::string_view assetPath) {
	return RunOp({
		"-Op=Components",
		"-Asset=" + std::string(assetPath),
	}).get<std::vector<BPComponent>>();
}

std::vector<BPNode>
SocketBlueprintReader::FindNode(std::string_view assetPath, std::string_view query,
							  std::string_view kind) {
	std::vector<std::string> args = {
		"-Op=Find",
		"-Asset=" + std::string(assetPath),
		"-Query=" + std::string(query),
	};
	if (!kind.empty())
	{
		args.push_back("-Kind=" + std::string(kind));
	}
	return RunOp(args).get<std::vector<BPNode>>();
}

// ----- write tools -------------------------------------------------------
// Same op-args the commandlet daemon already accepts. Editor-side
// dispatch happens on the game thread inside RunOneOp.

void SocketBlueprintReader::AddVariable(std::string_view a, std::string_view n,
									  const BPPinType& t, std::string_view dv,
									  std::string_view cat, bool repl, bool edit) {
	std::vector<std::string> args = {
		"-Op=AddVariable",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
		"-TypeCategory=" + t.Category,
	};
	if (t.SubCategory)
	{
		args.push_back("-TypeSubCategory=" + *t.SubCategory);
	}
	if (t.SubCategoryObject)
	{
		args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
	}
	if (t.IsArray)
	{
		args.push_back("-TypeIsArray");
	}
	if (t.IsSet)
	{
		args.push_back("-TypeIsSet");
	}
	if (t.IsMap)
	{
		args.push_back("-TypeIsMap");
	}
	if (!dv.empty())
	{
		args.push_back("-Default=" + std::string(dv));
	}
	if (!cat.empty())
	{
		args.push_back("-Category=" + std::string(cat));
	}
	if (repl)
	{
		args.push_back("-Replicated");
	}
	if (edit)
	{
		args.push_back("-Editable");
	}
	(void)RunOp(args);
}

void SocketBlueprintReader::SetNodePosition(std::string_view a, std::string_view g,
										  std::string_view n, int x, int y) {
	(void)RunOp({
		"-Op=SetNodePosition",
		"-Asset=" + std::string(a),
		"-Graph=" + std::string(g),
		"-Node=" + std::string(n),
		"-X=" + std::to_string(x),
		"-Y=" + std::to_string(y),
	});
}

void SocketBlueprintReader::DeleteNode(std::string_view a, std::string_view g,
									 std::string_view n) {
	(void)RunOp({
		"-Op=DeleteNode",
		"-Asset=" + std::string(a),
		"-Graph=" + std::string(g),
		"-Node=" + std::string(n),
	});
}

std::string SocketBlueprintReader::AddNode(std::string_view a, std::string_view g,
										 std::string_view k, int x, int y,
										 const std::map<std::string, std::string, std::less<>>& extras) {
	std::vector<std::string> args = {
		"-Op=AddNode",
		"-Asset=" + std::string(a),
		"-Graph=" + std::string(g),
		"-Kind=" + std::string(k),
		"-X=" + std::to_string(x),
		"-Y=" + std::to_string(y),
	};
	for (const auto& [key, val] : extras) {
		args.push_back("-" + key + "=" + val);
	}
	auto j = RunOp(args);
	return j.value("node_id", std::string{});
}

void SocketBlueprintReader::WirePins(std::string_view a, std::string_view g,
								   std::string_view fn, std::string_view fp,
								   std::string_view tn, std::string_view tp) {
	(void)RunOp({
		"-Op=WirePins",
		"-Asset=" + std::string(a),
		"-Graph=" + std::string(g),
		"-FromNode=" + std::string(fn),
		"-FromPin=" + std::string(fp),
		"-ToNode=" + std::string(tn),
		"-ToPin=" + std::string(tp),
	});
}

void SocketBlueprintReader::DeleteVariable(std::string_view a, std::string_view n) {
	(void)RunOp({
		"-Op=DeleteVariable",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
	});
}

void SocketBlueprintReader::RenameVariable(std::string_view a, std::string_view oldN,
										 std::string_view newN) {
	(void)RunOp({
		"-Op=RenameVariable",
		"-Asset=" + std::string(a),
		"-OldName=" + std::string(oldN),
		"-NewName=" + std::string(newN),
	});
}

IBlueprintReader::AddFunctionResult
SocketBlueprintReader::AddFunction(std::string_view a, std::string_view n) {
	auto j = RunOp({
		"-Op=AddFunction",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
	});
	AddFunctionResult out;
	out.functionName = j.value("function_name", std::string(n));
	out.entryNodeId  = j.value("entry_node_id", std::string{});
	return out;
}

void SocketBlueprintReader::AddFunctionInput(std::string_view a, std::string_view fn,
										   std::string_view p, const BPPinType& t) {
	std::vector<std::string> args = {
		"-Op=AddFunctionInput",
		"-Asset=" + std::string(a),
		"-Function=" + std::string(fn),
		"-Param=" + std::string(p),
		"-TypeCategory=" + t.Category,
	};
	if (t.SubCategory)
	{
		args.push_back("-TypeSubCategory=" + *t.SubCategory);
	}
	if (t.SubCategoryObject)
	{
		args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
	}
	if (t.IsArray)
	{
		args.push_back("-TypeIsArray");
	}
	if (t.IsSet)
	{
		args.push_back("-TypeIsSet");
	}
	if (t.IsMap)
	{
		args.push_back("-TypeIsMap");
	}
	(void)RunOp(args);
}

void SocketBlueprintReader::AddFunctionOutput(std::string_view a, std::string_view fn,
											std::string_view p, const BPPinType& t) {
	std::vector<std::string> args = {
		"-Op=AddFunctionOutput",
		"-Asset=" + std::string(a),
		"-Function=" + std::string(fn),
		"-Param=" + std::string(p),
		"-TypeCategory=" + t.Category,
	};
	if (t.SubCategory)
	{
		args.push_back("-TypeSubCategory=" + *t.SubCategory);
	}
	if (t.SubCategoryObject)
	{
		args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
	}
	if (t.IsArray)
	{
		args.push_back("-TypeIsArray");
	}
	if (t.IsSet)
	{
		args.push_back("-TypeIsSet");
	}
	if (t.IsMap)
	{
		args.push_back("-TypeIsMap");
	}
	(void)RunOp(args);
}

void SocketBlueprintReader::DeleteFunction(std::string_view a, std::string_view n) {
	(void)RunOp({
		"-Op=DeleteFunction",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
	});
}

void SocketBlueprintReader::SetVariableDefault(std::string_view a, std::string_view n,
											 std::string_view d) {
	std::vector<std::string> args = {
		"-Op=SetVariableDefault",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
	};
	if (!d.empty())
	{
		args.push_back("-Default=" + std::string(d));
	}
	(void)RunOp(args);
}

IBlueprintReader::CreateBlueprintResult
SocketBlueprintReader::CreateBlueprint(std::string_view a, std::string_view p) {
	auto j = RunOp({
		"-Op=CreateBlueprint",
		"-Asset=" + std::string(a),
		"-ParentClass=" + std::string(p),
	});
	CreateBlueprintResult out;
	out.alreadyExisted = j.value("already_existed", false);
	out.parentClass    = j.value("parent_class",    std::string{});
	return out;
}

void SocketBlueprintReader::SetPinDefault(std::string_view a, std::string_view g,
										std::string_view n, std::string_view pin,
										std::string_view v) {
	(void)RunOp({
		"-Op=SetPinDefault",
		"-Asset=" + std::string(a),
		"-Graph=" + std::string(g),
		"-Node=" + std::string(n),
		"-Pin=" + std::string(pin),
		"-Value=" + std::string(v),
	});
}

void SocketBlueprintReader::RetypeVariable(std::string_view a, std::string_view n,
										 const BPPinType& t) {
	std::vector<std::string> args = {
		"-Op=RetypeVariable",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
		"-TypeCategory=" + t.Category,
	};
	if (t.SubCategory)
	{
		args.push_back("-TypeSubCategory=" + *t.SubCategory);
	}
	if (t.SubCategoryObject)
	{
		args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
	}
	if (t.IsArray)
	{
		args.push_back("-TypeIsArray");
	}
	if (t.IsSet)
	{
		args.push_back("-TypeIsSet");
	}
	if (t.IsMap)
	{
		args.push_back("-TypeIsMap");
	}
	(void)RunOp(args);
}

void SocketBlueprintReader::SetVariableCategory(std::string_view a, std::string_view n,
											  std::string_view category) {
	std::vector<std::string> args = {
		"-Op=SetVariableCategory",
		"-Asset=" + std::string(a),
		"-Name=" + std::string(n),
	};
	if (!category.empty())
	{
		args.push_back("-Category=" + std::string(category));
	}
	(void)RunOp(args);
}

IBlueprintReader::WriteGeneratedSourceResult
SocketBlueprintReader::WriteGeneratedSource(std::string_view destPath,
										  std::string_view content,
										  bool createDirs) {
	// Same temp-file trick the commandlet uses: the wire frame format
	// would technically let us send content inline (it's JSON-shaped,
	// not line-bounded), but we keep symmetry with the commandlet so
	// both backends share the plugin op's calling convention.
	namespace fs = std::filesystem;
	fs::path tempDir = fs::temp_directory_path();
	fs::path contentTemp = tempDir /
		("bpr-live-write-" + std::to_string(static_cast<unsigned long long>(
			std::hash<std::string>{}(std::string(destPath)))) + ".txt");
	{
		std::ofstream f(contentTemp, std::ios::binary);
		f.write(content.data(), static_cast<std::streamsize>(content.size()));
	}

	std::vector<std::string> args = {
		"-Op=WriteGeneratedSource",
		"-Path=" + std::string(destPath),
		"-ContentFile=" + contentTemp.string(),
	};
	if (createDirs)
	{
		args.push_back("-CreateDirs");
	}
	auto j = RunOp(args);

	std::error_code ec;
	fs::remove(contentTemp, ec);

	WriteGeneratedSourceResult out;
	if (j.is_object()) {
		out.bytesWritten = j.value("bytes_written", std::size_t{0});
		out.path         = j.value("path", std::string{});
	}
	return out;
}

IBlueprintReader::DuplicateBlueprintResult
SocketBlueprintReader::DuplicateBlueprint(std::string_view source, std::string_view dest) {
	auto j = RunOp({
		"-Op=DuplicateBlueprint",
		"-Asset=" + std::string(source),
		"-Dest="  + std::string(dest),
	});
	DuplicateBlueprintResult out;
	out.alreadyExisted   = j.is_object() && j.value("already_existed", false);
	out.sourceAssetPath  = std::string(source);
	return out;
}

nlohmann::json SocketBlueprintReader::StructuralDiff(
	std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
	std::vector<std::string> args = {
		"-Op=StructuralDiff",
		"-A=" + std::string(a),
		"-B=" + std::string(b),
	};
	// Mirror CommandletBlueprintReader::StructuralDiff: only emit the
	// flag when the caller wants to opt out of the plugin default.
	if (!opts.ignoreNodePositions) {
		args.push_back("-IgnoreNodePositions=0");
	}
	if (opts.ignoreCommentNodes) {
		args.push_back("-IgnoreCommentNodes");
	}
	return RunOp(args);
}

namespace {
IBlueprintReader::AssetRegistryListResult
ParseAssetRegistryRows(const nlohmann::json& j, const char* arrayKey) {
	IBlueprintReader::AssetRegistryListResult out;
	if (!j.is_object()) {
		return out;
	}
	auto it = j.find(arrayKey);
	if (it == j.end() || !it->is_array()) {
		return out;
	}
	out.entries.reserve(it->size());
	for (const auto& row : *it) {
		if (!row.is_object()) continue;
		IBlueprintReader::AssetRegistryEntry e;
		e.assetPath = row.value("asset_path", std::string{});
		e.name      = row.value("name",       std::string{});
		e.className = row.value("class_name", std::string{});
		out.entries.push_back(std::move(e));
	}
	return out;
}
}    // namespace

IBlueprintReader::AssetRegistryListResult
SocketBlueprintReader::ListAssets(std::string_view path, bool recursive) {
	std::vector<std::string> args = {"-Op=ListAssets"};
	if (!path.empty()) {
		args.push_back("-Path=" + std::string(path));
	}
	if (!recursive) {
		args.push_back("-NonRecursive");
	}
	return ParseAssetRegistryRows(RunOp(args), "assets");
}

IBlueprintReader::AssetRegistryListResult
SocketBlueprintReader::FindAsset(std::string_view query, std::string_view path) {
	std::vector<std::string> args = {
		"-Op=FindAsset",
		"-Query=" + std::string(query),
	};
	if (!path.empty()) {
		args.push_back("-Path=" + std::string(path));
	}
	return ParseAssetRegistryRows(RunOp(args), "matches");
}

// ----- batch sentinels ---------------------------------------------------
// ----- Project + Content Browser ops ---------------------------------

IBlueprintReader::ProjectMetadata
SocketBlueprintReader::GetProjectMetadata() {
	// Project metadata is a pure local file read — same data flow as
	// CommandletBlueprintReader::GetProjectMetadata. Going over the
	// wire returns code=1 because the editor side doesn't implement a
	// `-Op=GetProjectMetadata` handler (see BlueprintReaderCommandlet.cpp
	// — it would be redundant: GEditor knows the project path, but the
	// MCP server's cfg already knows it via Config.projectPath, so we
	// skip the round-trip entirely).
	ProjectMetadata out;
	if (cfg_.projectPath.empty()) {
		// Caller never wired projectPath into Config — fall back to
		// throwing the same shape CommandletBlueprintReader does so
		// the MCP tool layer surfaces a coherent error.
		throw BlueprintReaderError(
			"GetProjectMetadata: SocketBlueprintReader has no projectPath "
			"configured. Set BP_READER_PROJECT in the MCP server env, "
			"or use the auto backend (which auto-discovers it).");
	}
	out.projectPath = cfg_.projectPath;
	// Derive name from filename stem (everything after the last / or \
	// up to the final '.', mirroring std::filesystem::path::stem()).
	{
		const auto& p = out.projectPath;
		auto lastSlash = p.find_last_of("/\\");
		auto stemStart = (lastSlash == std::string::npos) ? 0 : lastSlash + 1;
		auto lastDot = p.find_last_of('.');
		auto stemEnd = (lastDot != std::string::npos && lastDot > stemStart)
			? lastDot : p.size();
		out.projectName = p.substr(stemStart, stemEnd - stemStart);
	}
	std::ifstream f(out.projectPath);
	if (!f) {
		throw BlueprintReaderError(fmt::format(
			"GetProjectMetadata: cannot read {}", out.projectPath));
	}
	std::stringstream ss;
	ss << f.rdbuf();
	try {
		out.raw = nlohmann::json::parse(ss.str());
	} catch (const std::exception& e) {
		throw BlueprintReaderError(fmt::format(
			"GetProjectMetadata: {} is not valid JSON ({})",
			out.projectPath, e.what()));
	}
	if (out.raw.is_object()) {
		out.engineAssociation = out.raw.value("EngineAssociation", std::string{});
		out.category          = out.raw.value("Category",          std::string{});
		out.description       = out.raw.value("Description",       std::string{});
	}
	return out;
}

IBlueprintReader::SaveAllResult
SocketBlueprintReader::SaveAll(bool dirtyOnly) {
	std::vector<std::string> args = {"-Op=SaveAll"};
	if (!dirtyOnly)
	{
		args.push_back("-IncludeClean");
	}
	auto j = RunOp(args);
	SaveAllResult out;
	if (j.is_object()) {
		out.savedCount = j.value("saved_count", 0);
		if (auto it = j.find("failed_assets"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string())
				{
					out.failedAssets.push_back(v.get<std::string>());
				}
			}
		}
	}
	return out;
}

IBlueprintReader::MoveAssetResult
SocketBlueprintReader::MoveAsset(std::string_view sourcePath,
							   std::string_view destPath) {
	auto j = RunOp({
		"-Op=MoveAsset",
		"-Asset=" + std::string(sourcePath),
		"-Dest="  + std::string(destPath),
	});
	MoveAssetResult out;
	out.sourcePath = std::string(sourcePath);
	out.destPath   = std::string(destPath);
	if (j.is_object()) {
		out.redirectorsCreated = j.value("redirectors_created", 0);
	}
	return out;
}

IBlueprintReader::DeleteAssetResult
SocketBlueprintReader::DeleteAsset(std::string_view assetPath, bool force) {
	std::vector<std::string> args = {
		"-Op=DeleteAsset",
		"-Asset=" + std::string(assetPath),
	};
	if (force)
	{
		args.push_back("-Force");
	}
	auto j = RunOp(args);
	DeleteAssetResult out;
	out.path = std::string(assetPath);
	if (j.is_object()) {
		out.deleted = j.value("deleted", false);
		if (auto it = j.find("referencing_assets"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string())
				{
					out.referencingAssets.push_back(v.get<std::string>());
				}
			}
		}
	}
	return out;
}

IBlueprintReader::CreateFolderResult
SocketBlueprintReader::CreateFolder(std::string_view folderPath) {
	auto j = RunOp({
		"-Op=CreateFolder",
		"-Path=" + std::string(folderPath),
	});
	CreateFolderResult out;
	out.path = std::string(folderPath);
	if (j.is_object()) {
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

std::vector<BPAssetSummary>
SocketBlueprintReader::ListDataTables(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListDataTables"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) {
			BPAssetSummary s;
			from_json(v, s);
			out.push_back(std::move(s));
		}
	}
	return out;
}

IBlueprintReader::DataTableInfo
SocketBlueprintReader::ReadDataTable(std::string_view assetPath) {
	auto j = RunOp({
		"-Op=ReadDataTable",
		"-Asset=" + std::string(assetPath),
	});
	DataTableInfo out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.rowStruct = j.value("row_struct", std::string{});
		if (auto it = j.find("columns"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string())
				{
					out.columns.push_back(v.get<std::string>());
				}
			}
		}
		if (auto it = j.find("rows"); it != j.end() && it->is_array()) {
			out.rows = *it;
		}
	}
	return out;
}

IBlueprintReader::AddDataRowResult
SocketBlueprintReader::AddDataRow(std::string_view assetPath,
								std::string_view rowName,
								const nlohmann::json& values,
								bool overwrite) {
	namespace fs = std::filesystem;
	fs::path tempDir = fs::temp_directory_path();
	fs::path valuesTemp = tempDir /
		("bpr-live-add-row-" + std::to_string(static_cast<unsigned long long>(
			std::hash<std::string>{}(std::string(assetPath) + ":" +
									  std::string(rowName)))) + ".json");
	{
		std::ofstream f(valuesTemp);
		f << values.dump();
	}
	std::vector<std::string> args = {
		"-Op=AddDataRow",
		"-Asset=" + std::string(assetPath),
		"-Row="   + std::string(rowName),
		"-ValuesFile=" + valuesTemp.string(),
	};
	if (overwrite)
	{
		args.push_back("-Overwrite");
	}
	auto j = RunOp(args);
	std::error_code ec;
	fs::remove(valuesTemp, ec);

	AddDataRowResult out;
	out.assetPath = std::string(assetPath);
	out.rowName   = std::string(rowName);
	if (j.is_object()) {
		out.alreadyExisted = j.value("already_existed", false);
		out.created        = j.value("created", false);
	}
	return out;
}

IBlueprintReader::SetDataRowValueResult
SocketBlueprintReader::SetDataRowValue(std::string_view assetPath,
									 std::string_view rowName,
									 std::string_view fieldName,
									 std::string_view value) {
	auto j = RunOp({
		"-Op=SetDataRowValue",
		"-Asset=" + std::string(assetPath),
		"-Row="   + std::string(rowName),
		"-Field=" + std::string(fieldName),
		"-Value=" + std::string(value),
	});
	SetDataRowValueResult out;
	out.assetPath = std::string(assetPath);
	out.rowName   = std::string(rowName);
	out.fieldName = std::string(fieldName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

// ----- Component (SCS) authoring -----------------------------------------

IBlueprintReader::AddComponentResult
SocketBlueprintReader::AddComponent(std::string_view assetPath,
								  std::string_view name,
								  std::string_view componentClass,
								  std::string_view parentName,
								  std::string_view socket) {
	std::vector<std::string> args = {
		"-Op=AddComponent",
		"-Asset=" + std::string(assetPath),
		"-Name="  + std::string(name),
		"-Class=" + std::string(componentClass),
	};
	if (!parentName.empty())
	{
		args.push_back("-Parent=" + std::string(parentName));
	}
	if (!socket.empty())
	{
		args.push_back("-Socket=" + std::string(socket));
	}
	auto j = RunOp(args);
	AddComponentResult out;
	out.assetPath      = std::string(assetPath);
	out.name           = std::string(name);
	out.componentClass = std::string(componentClass);
	if (j.is_object()) {
		out.alreadyExisted = j.value("already_existed", false);
		out.created        = j.value("created", false);
	}
	return out;
}

IBlueprintReader::RemoveComponentResult
SocketBlueprintReader::RemoveComponent(std::string_view assetPath,
									 std::string_view name) {
	auto j = RunOp({"-Op=RemoveComponent",
					"-Asset=" + std::string(assetPath),
					"-Name="  + std::string(name)});
	RemoveComponentResult out;
	out.assetPath = std::string(assetPath);
	out.name      = std::string(name);
	if (j.is_object())
	{
		out.removed = j.value("removed", false);
	}
	return out;
}

IBlueprintReader::AttachComponentResult
SocketBlueprintReader::AttachComponent(std::string_view assetPath,
									 std::string_view name,
									 std::string_view newParentName,
									 std::string_view socket) {
	std::vector<std::string> args = {
		"-Op=AttachComponent",
		"-Asset=" + std::string(assetPath),
		"-Name="  + std::string(name),
	};
	if (!newParentName.empty())
	{
		args.push_back("-NewParent=" + std::string(newParentName));
	}
	if (!socket.empty())
	{
		args.push_back("-Socket="    + std::string(socket));
	}
	auto j = RunOp(args);
	AttachComponentResult out;
	out.assetPath     = std::string(assetPath);
	out.name          = std::string(name);
	out.newParentName = std::string(newParentName);
	out.socket        = std::string(socket);
	if (j.is_object())
	{
		out.reparented = j.value("reparented", false);
	}
	return out;
}

IBlueprintReader::SetComponentPropertyResult
SocketBlueprintReader::SetComponentProperty(std::string_view assetPath,
										  std::string_view componentName,
										  std::string_view propertyName,
										  std::string_view value) {
	auto j = RunOp({"-Op=SetComponentProperty",
					"-Asset="     + std::string(assetPath),
					"-Component=" + std::string(componentName),
					"-Property="  + std::string(propertyName),
					"-Value="     + std::string(value)});
	SetComponentPropertyResult out;
	out.assetPath     = std::string(assetPath);
	out.componentName = std::string(componentName);
	out.propertyName  = std::string(propertyName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

// ----- Live editor ops ---------------------------------------------------

IBlueprintReader::ConsoleCommandResult
SocketBlueprintReader::ConsoleCommand(std::string_view command) {
	auto j = RunOp({"-Op=ConsoleCommand",
					"-Command=" + std::string(command)});
	ConsoleCommandResult out;
	if (j.is_object()) out.output = j.value("output", std::string{});
	return out;
}

IBlueprintReader::CVarValue
SocketBlueprintReader::GetCVar(std::string_view name) {
	auto j = RunOp({"-Op=GetCVar", "-Name=" + std::string(name)});
	CVarValue out;
	out.name = std::string(name);
	if (j.is_object()) {
		out.value  = j.value("value",  std::string{});
		out.help   = j.value("help",   std::string{});
		out.exists = j.value("exists", false);
	}
	return out;
}

IBlueprintReader::CVarValue
SocketBlueprintReader::SetCVar(std::string_view name, std::string_view value) {
	auto j = RunOp({"-Op=SetCVar",
					"-Name="  + std::string(name),
					"-Value=" + std::string(value)});
	CVarValue out;
	out.name = std::string(name);
	if (j.is_object()) {
		out.value  = j.value("value",  std::string{});
		out.help   = j.value("help",   std::string{});
		out.exists = j.value("exists", false);
	}
	return out;
}

IBlueprintReader::PieResult
SocketBlueprintReader::PieStart(std::string_view mode) {
	std::vector<std::string> args = {"-Op=PieStart"};
	if (!mode.empty())
	{
		args.push_back("-Mode=" + std::string(mode));
	}
	auto j = RunOp(args);
	PieResult out;
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.mode    = j.value("mode",    std::string{});
	}
	return out;
}

IBlueprintReader::PieResult SocketBlueprintReader::PieStop() {
	auto j = RunOp({"-Op=PieStop"});
	PieResult out;
	if (j.is_object())
	{
		out.stopped = j.value("stopped", false);
	}
	return out;
}

// ----- Phase 8 EA-pull Wave 1 (partial) -----------------------------

IBlueprintReader::OpenAssetsResult SocketBlueprintReader::ListOpenAssets() {
	auto j = RunOp({"-Op=ListOpenAssets"});
	OpenAssetsResult out;
	if (j.is_object()) {
		if (auto it = j.find("entries"); it != j.end() && it->is_array()) {
			for (const auto& e : *it) {
				if (!e.is_object()) continue;
				OpenAssetInfo info;
				info.assetPath  = e.value("asset_path",  std::string{});
				info.assetClass = e.value("asset_class", std::string{});
				info.lastActivationSeconds =
					e.value("last_activation_seconds", 0.0);
				out.entries.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::ActiveAssetResult SocketBlueprintReader::GetActiveAsset() {
	auto j = RunOp({"-Op=GetActiveAsset"});
	ActiveAssetResult out;
	if (j.is_object()) {
		out.assetPath  = j.value("asset_path",  std::string{});
		out.assetClass = j.value("asset_class", std::string{});
		out.lastActivationSeconds =
			j.value("last_activation_seconds", 0.0);
	}
	return out;
}

IBlueprintReader::CompileStatusResult
SocketBlueprintReader::GetCompileStatus(std::string_view assetPath) {
	auto j = RunOp({"-Op=GetCompileStatus",
					"-Asset=" + std::string(assetPath)});
	CompileStatusResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.status           = j.value("status", std::string{});
		out.lastCompileError = j.value("last_compile_error", std::string{});
	}
	return out;
}

IBlueprintReader::DirtyPackagesResult
SocketBlueprintReader::GetDirtyPackages() {
	auto j = RunOp({"-Op=GetDirtyPackages"});
	DirtyPackagesResult out;
	if (j.is_object()) {
		if (auto it = j.find("packages"); it != j.end() && it->is_array()) {
			for (const auto& p : *it) {
				if (!p.is_object()) continue;
				DirtyPackageInfo info;
				info.packageName      = p.value("package_name", std::string{});
				info.isContentPackage = p.value("is_content",   false);
				out.packages.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::FocusedWindowResult
SocketBlueprintReader::GetFocusedWindow() {
	auto j = RunOp({"-Op=GetFocusedWindow"});
	FocusedWindowResult out;
	if (j.is_object()) {
		out.title     = j.value("title",      std::string{});
		out.className = j.value("class_name", std::string{});
	}
	return out;
}

IBlueprintReader::PieStateResult SocketBlueprintReader::GetPieState() {
	auto j = RunOp({"-Op=GetPieState"});
	PieStateResult out;
	if (j.is_object()) {
		out.isPlaying     = j.value("is_playing",     false);
		out.mode          = j.value("mode",           std::string{});
		out.instanceCount = j.value("instance_count", 0);
	}
	return out;
}

IBlueprintReader::ModalStateResult SocketBlueprintReader::GetModalState() {
	auto j = RunOp({"-Op=GetModalState"});
	ModalStateResult out;
	if (j.is_object()) {
		out.isOpen = j.value("is_open", false);
		out.title  = j.value("title",   std::string{});
	}
	return out;
}

IBlueprintReader::EditorModesResult
SocketBlueprintReader::GetActiveEditorMode() {
	auto j = RunOp({"-Op=GetActiveEditorMode"});
	EditorModesResult out;
	if (j.is_object()) {
		if (auto it = j.find("active_modes"); it != j.end() && it->is_array()) {
			for (const auto& m : *it) {
				if (m.is_string()) {
					out.activeModes.push_back(m.get<std::string>());
				}
			}
		}
	}
	return out;
}

IBlueprintReader::FocusedWidgetResult
SocketBlueprintReader::GetFocusedWidget() {
	auto j = RunOp({"-Op=GetFocusedWidget"});
	FocusedWidgetResult out;
	if (j.is_object()) {
		out.widgetType        = j.value("widget_type",         std::string{});
		out.parentWindowTitle = j.value("parent_window_title", std::string{});
	}
	return out;
}

IBlueprintReader::OpenAssetEditorResult
SocketBlueprintReader::OpenAssetEditor(std::string_view assetPath) {
	auto j = RunOp({"-Op=OpenAssetEditor",
					std::string("-Asset=") + std::string(assetPath)});
	OpenAssetEditorResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.opened = j.value("opened", false);
	}
	return out;
}

IBlueprintReader::CloseAssetEditorResult
SocketBlueprintReader::CloseAssetEditor(std::string_view assetPath) {
	auto j = RunOp({"-Op=CloseAssetEditor",
					std::string("-Asset=") + std::string(assetPath)});
	CloseAssetEditorResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.closed = j.value("closed", false);
	}
	return out;
}

IBlueprintReader::CameraTransformResult
SocketBlueprintReader::GetCameraTransform() {
	auto j = RunOp({"-Op=GetCameraTransform"});
	CameraTransformResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.locX  = j.value("loc_x",  0.0);
		out.locY  = j.value("loc_y",  0.0);
		out.locZ  = j.value("loc_z",  0.0);
		out.pitch = j.value("pitch",  0.0);
		out.yaw   = j.value("yaw",    0.0);
		out.roll  = j.value("roll",   0.0);
		out.fov   = j.value("fov",    0.0);
	}
	return out;
}

IBlueprintReader::ViewModeResult
SocketBlueprintReader::GetViewMode() {
	auto j = RunOp({"-Op=GetViewMode"});
	ViewModeResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.mode  = j.value("mode",  std::string{});
	}
	return out;
}

IBlueprintReader::ShowFlagsResult
SocketBlueprintReader::GetShowFlags() {
	auto j = RunOp({"-Op=GetShowFlags"});
	ShowFlagsResult out;
	if (j.is_object()) {
		out.valid          = j.value("valid",          false);
		out.wireframe      = j.value("wireframe",      false);
		out.collision      = j.value("collision",      false);
		out.grid           = j.value("grid",           false);
		out.bounds         = j.value("bounds",         false);
		out.navigation     = j.value("navigation",     false);
		out.atmosphere     = j.value("atmosphere",     false);
		out.fog            = j.value("fog",            false);
		out.lighting       = j.value("lighting",       false);
		out.postProcessing = j.value("post_processing",false);
		out.antialiasing   = j.value("antialiasing",   false);
		out.shadows        = j.value("shadows",        false);
	}
	return out;
}

IBlueprintReader::LiveCodingResult
SocketBlueprintReader::LiveCodingCompile() {
	auto j = RunOp({"-Op=LiveCodingCompile"});
	LiveCodingResult out;
	if (j.is_object()) {
		out.queued  = j.value("queued",  false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::SelectionResult
SocketBlueprintReader::GetSelectedActors() {
	auto j = RunOp({"-Op=GetSelectedActors"});
	SelectionResult out;
	if (j.is_object()) {
		if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string())
				{
					out.actorNames.push_back(v.get<std::string>());
				}
			}
		}
	}
	return out;
}

nlohmann::json SocketBlueprintReader::GetEditorState() {
	auto j = RunOp({"-Op=GetEditorState"});
	if (j.is_object())
	{
		j.erase("ok");
	}
	return j;
}

IBlueprintReader::PythonResult
SocketBlueprintReader::RunPythonScript(std::string_view code) {
	auto j = RunOp({"-Op=RunPythonScript",
					"-Code=" + std::string(code)});
	PythonResult out;
	if (j.is_object()) {
		if (auto it = j.find("ok"); it != j.end() && it->is_boolean())
		{
				out.ok = it->get<bool>();
		}
		if (auto it = j.find("error"); it != j.end() && it->is_string())
		{
				out.error = it->get<std::string>();
		}
		if (auto it = j.find("command_result"); it != j.end() && it->is_string())
		{
				out.commandResult = it->get<std::string>();
		}
		if (auto it = j.find("log"); it != j.end())
		{
				out.log = *it;
		}
	}
	return out;
}

// Helper for asset-graph + similar list-shaped responses.
static std::vector<std::string> SocketExtractStringArray(
	const nlohmann::json& j, const char* field) {
	std::vector<std::string> out;
	if (!j.is_object())
	{
		return out;
	}
	auto it = j.find(field);
	if (it == j.end() || !it->is_array())
	{
		return out;
	}
	out.reserve(it->size());
	for (const auto& v : *it) {
		if (v.is_string())
		{
			out.push_back(v.get<std::string>());
		}
	}
	return out;
}

IBlueprintReader::AssetGraphResult
SocketBlueprintReader::GetReferencers(std::string_view assetPath) {
	auto j = RunOp({"-Op=GetReferencers", "-Asset=" + std::string(assetPath)});
	AssetGraphResult out;
	out.packagePaths = SocketExtractStringArray(j, "referencers");
	return out;
}

IBlueprintReader::AssetGraphResult
SocketBlueprintReader::GetDependencies(std::string_view assetPath) {
	auto j = RunOp({"-Op=GetDependencies", "-Asset=" + std::string(assetPath)});
	AssetGraphResult out;
	out.packagePaths = SocketExtractStringArray(j, "dependencies");
	return out;
}

IBlueprintReader::ConfigReadResult
SocketBlueprintReader::ReadConfigValue(std::string_view section,
									   std::string_view key,
									   std::string_view file) {
	auto j = RunOp({"-Op=ReadConfigValue",
					"-Section=" + std::string(section),
					"-Key=" + std::string(key),
					"-File=" + std::string(file)});
	ConfigReadResult out;
	if (j.is_object()) {
		if (auto it = j.find("exists"); it != j.end() && it->is_boolean())
		{
				out.exists = it->get<bool>();
		}
		if (auto it = j.find("value"); it != j.end() && it->is_string())
		{
				out.value = it->get<std::string>();
		}
	}
	return out;
}

IBlueprintReader::ConfigWriteResult
SocketBlueprintReader::SetConfigValue(std::string_view section,
									  std::string_view key,
									  std::string_view value,
									  std::string_view file) {
	auto j = RunOp({"-Op=SetConfigValue",
					"-Section=" + std::string(section),
					"-Key=" + std::string(key),
					"-Value=" + std::string(value),
					"-File=" + std::string(file)});
	ConfigWriteResult out;
	if (j.is_object()) {
		if (auto it = j.find("previous_value"); it != j.end()) {
			if (it->is_string()) {
				out.previousExisted = true;
				out.previousValue = it->get<std::string>();
			}
		}
	}
	return out;
}

IBlueprintReader::BuildLightingResult
SocketBlueprintReader::BuildLighting(std::string_view quality) {
	auto j = RunOp({"-Op=BuildLighting", "-Quality=" + std::string(quality)});
	BuildLightingResult out;
	if (j.is_object()) {
		if (auto it = j.find("queued"); it != j.end() && it->is_boolean())
		{
				out.queued = it->get<bool>();
		}
		if (auto it = j.find("quality"); it != j.end() && it->is_string())
		{
				out.quality = it->get<std::string>();
		}
	}
	return out;
}

IBlueprintReader::SelectionResult
SocketBlueprintReader::SetSelection(const std::vector<std::string>& actorNames,
								  bool replace) {
	std::string joined;
	for (std::size_t i = 0; i < actorNames.size(); ++i) {
		if (i)
		{
			joined += ",";
		}
		joined += actorNames[i];
	}
	std::vector<std::string> args = {"-Op=SetSelection", "-Names=" + joined};
	if (!replace)
	{
		args.push_back("-Add");
	}
	auto j = RunOp(args);
	SelectionResult out;
	if (j.is_object()) {
		if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string())
				{
					out.actorNames.push_back(v.get<std::string>());
				}
			}
		}
	}
	return out;
}

IBlueprintReader::SpawnActorResult
SocketBlueprintReader::SpawnActor(std::string_view classPath,
	double locX, double locY, double locZ,
	double rotPitch, double rotYaw, double rotRoll,
	double scaleX, double scaleY, double scaleZ) {
	auto j = RunOp({
		"-Op=SpawnActor",
		"-Class=" + std::string(classPath),
		"-LocX=" + std::to_string(locX),
		"-LocY=" + std::to_string(locY),
		"-LocZ=" + std::to_string(locZ),
		"-RotPitch=" + std::to_string(rotPitch),
		"-RotYaw="   + std::to_string(rotYaw),
		"-RotRoll="  + std::to_string(rotRoll),
		"-ScaleX=" + std::to_string(scaleX),
		"-ScaleY=" + std::to_string(scaleY),
		"-ScaleZ=" + std::to_string(scaleZ),
	});
	SpawnActorResult out;
	if (j.is_object()) {
		out.actorName  = j.value("actor_name",  std::string{});
		out.actorLabel = j.value("actor_label", std::string{});
	}
	return out;
}

void SocketBlueprintReader::SetActorTransform(std::string_view actorName,
	double locX, double locY, double locZ,
	double rotPitch, double rotYaw, double rotRoll,
	double scaleX, double scaleY, double scaleZ) {
	(void)RunOp({
		"-Op=SetActorTransform",
		"-Name=" + std::string(actorName),
		"-LocX=" + std::to_string(locX),
		"-LocY=" + std::to_string(locY),
		"-LocZ=" + std::to_string(locZ),
		"-RotPitch=" + std::to_string(rotPitch),
		"-RotYaw="   + std::to_string(rotYaw),
		"-RotRoll="  + std::to_string(rotRoll),
		"-ScaleX=" + std::to_string(scaleX),
		"-ScaleY=" + std::to_string(scaleY),
		"-ScaleZ=" + std::to_string(scaleZ),
	});
}

IBlueprintReader::DeleteActorResult
SocketBlueprintReader::DeleteActor(std::string_view actorName) {
	auto j = RunOp({"-Op=DeleteActor", "-Name=" + std::string(actorName)});
	DeleteActorResult out;
	if (j.is_object())
	{
		out.deleted = j.value("deleted", false);
	}
	return out;
}

IBlueprintReader::OutputLogResult
SocketBlueprintReader::ReadOutputLog(int limit, std::string_view minSeverity) {
	std::vector<std::string> args = {"-Op=ReadOutputLog",
									  "-Limit=" + std::to_string(limit)};
	if (!minSeverity.empty()) {
		args.push_back("-MinSeverity=" + std::string(minSeverity));
	}
	auto j = RunOp(args);
	OutputLogResult out;
	if (j.is_object()) {
		if (auto it = j.find("entries"); it != j.end() && it->is_array()) {
			for (const auto& e : *it) {
				if (!e.is_object())
				{
					continue;
				}
				LogEntry entry;
				entry.severity  = e.value("severity",  std::string{});
				entry.category  = e.value("category",  std::string{});
				entry.message   = e.value("message",   std::string{});
				entry.timestamp = e.value("timestamp", std::string{});
				out.entries.push_back(std::move(entry));
			}
		}
	}
	return out;
}

IBlueprintReader::AutomationRunResult
SocketBlueprintReader::RunAutomationTests(std::string_view pattern) {
	std::vector<std::string> args = {"-Op=RunAutomationTests"};
	if (!pattern.empty())
	{
		args.push_back("-Pattern=" + std::string(pattern));
	}
	auto j = RunOp(args);
	AutomationRunResult out;
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

// ----- Material authoring ------------------------------------------------

std::vector<BPAssetSummary>
SocketBlueprintReader::ListMaterials(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListMaterials"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) {
			BPAssetSummary s; from_json(v, s); out.push_back(std::move(s));
		}
	}
	return out;
}

IBlueprintReader::MaterialInfo
SocketBlueprintReader::ReadMaterial(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadMaterial", "-Asset=" + std::string(assetPath)});
	MaterialInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	if (auto it = j.find("expressions"); it != j.end() && it->is_array()) {
		for (const auto& e : *it) {
			MaterialExpression x;
			x.id            = e.value("id",             std::string{});
			x.className     = e.value("class",          std::string{});
			x.parameterName = e.value("parameter_name", std::string{});
			x.x             = e.value("x", 0);
			x.y             = e.value("y", 0);
			out.expressions.push_back(std::move(x));
		}
	}
	if (auto it = j.find("connections"); it != j.end() && it->is_array()) {
		for (const auto& c : *it) {
			MaterialConnection mc;
			mc.fromNodeId = c.value("from_node", std::string{});
			mc.fromPin    = c.value("from_pin",  std::string{});
			mc.toNodeId   = c.value("to_node",   std::string{});
			mc.toPin      = c.value("to_pin",    std::string{});
			out.connections.push_back(std::move(mc));
		}
	}
	if (auto it = j.find("parameter_names"); it != j.end() && it->is_array()) {
		for (const auto& v : *it) {
			if (v.is_string())
			{
				out.parameterNames.push_back(v.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::AddMaterialExpressionResult
SocketBlueprintReader::AddMaterialExpression(std::string_view assetPath,
	std::string_view expressionClass, int x, int y) {
	auto j = RunOp({"-Op=AddMaterialExpression",
					"-Asset=" + std::string(assetPath),
					"-Class=" + std::string(expressionClass),
					"-X=" + std::to_string(x),
					"-Y=" + std::to_string(y)});
	AddMaterialExpressionResult out;
	out.assetPath = std::string(assetPath);
	out.className = std::string(expressionClass);
	if (j.is_object()) out.expressionId = j.value("expression_id", std::string{});
	return out;
}

IBlueprintReader::ConnectMaterialResult
SocketBlueprintReader::ConnectMaterialExpressions(std::string_view assetPath,
	std::string_view fromNodeId, std::string_view fromPin,
	std::string_view toNodeId, std::string_view toPin) {
	auto j = RunOp({"-Op=ConnectMaterialExpressions",
					"-Asset="   + std::string(assetPath),
					"-From="    + std::string(fromNodeId),
					"-FromPin=" + std::string(fromPin),
					"-To="      + std::string(toNodeId),
					"-ToPin="   + std::string(toPin)});
	ConnectMaterialResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.connected = j.value("connected", false);
	}
	return out;
}

IBlueprintReader::SetMaterialParameterResult
SocketBlueprintReader::SetMaterialParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view value) {
	auto j = RunOp({"-Op=SetMaterialParameter",
					"-Asset=" + std::string(assetPath),
					"-Param=" + std::string(parameterName),
					"-Value=" + std::string(value)});
	SetMaterialParameterResult out;
	out.assetPath = std::string(assetPath);
	out.parameterName = std::string(parameterName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

IBlueprintReader::SetMIParameterResult
SocketBlueprintReader::SetMaterialInstanceParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view paramType,
	std::string_view value) {
	auto j = RunOp({"-Op=SetMaterialInstanceParameter",
					"-Asset=" + std::string(assetPath),
					"-Param=" + std::string(parameterName),
					"-Type="  + std::string(paramType),
					"-Value=" + std::string(value)});
	SetMIParameterResult out;
	out.assetPath     = std::string(assetPath);
	out.parameterName = std::string(parameterName);
	out.paramType     = std::string(paramType);
	if (j.is_object()) out.newValue = j.value("new_value", std::string{});
	return out;
}

IBlueprintReader::CompileMaterialResult
SocketBlueprintReader::CompileMaterial(std::string_view assetPath) {
	auto j = RunOp({"-Op=CompileMaterial", "-Asset=" + std::string(assetPath)});
	CompileMaterialResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

// ----- UMG widget authoring ----------------------------------------------

IBlueprintReader::WidgetBlueprintInfo
SocketBlueprintReader::ReadWidgetBlueprint(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadWidgetBlueprint", "-Asset=" + std::string(assetPath)});
	WidgetBlueprintInfo out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.rootName = j.value("root_name", std::string{});
		if (auto it = j.find("nodes"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				WidgetNode wn;
				wn.name       = n.value("name",   std::string{});
				wn.className  = n.value("class",  std::string{});
				wn.parentName = n.value("parent", std::string{});
				out.nodes.push_back(std::move(wn));
			}
		}
	}
	return out;
}

IBlueprintReader::AddWidgetResult
SocketBlueprintReader::AddWidget(std::string_view assetPath,
	std::string_view parentName, std::string_view widgetClass,
	std::string_view name) {
	std::vector<std::string> args = {"-Op=AddWidget",
									 "-Asset=" + std::string(assetPath),
									 "-Name="  + std::string(name),
									 "-Class=" + std::string(widgetClass)};
	if (!parentName.empty())
	{
		args.push_back("-Parent=" + std::string(parentName));
	}
	auto j = RunOp(args);
	AddWidgetResult out;
	out.assetPath   = std::string(assetPath);
	out.name        = std::string(name);
	out.widgetClass = std::string(widgetClass);
	if (j.is_object()) {
		out.alreadyExisted = j.value("already_existed", false);
		out.created        = j.value("created", false);
	}
	return out;
}

IBlueprintReader::SetWidgetPropertyResult
SocketBlueprintReader::SetWidgetProperty(std::string_view assetPath,
	std::string_view widgetName, std::string_view propertyName,
	std::string_view value) {
	auto j = RunOp({"-Op=SetWidgetProperty",
					"-Asset="    + std::string(assetPath),
					"-Widget="   + std::string(widgetName),
					"-Property=" + std::string(propertyName),
					"-Value="    + std::string(value)});
	SetWidgetPropertyResult out;
	out.assetPath    = std::string(assetPath);
	out.widgetName   = std::string(widgetName);
	out.propertyName = std::string(propertyName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

IBlueprintReader::BindWidgetEventResult
SocketBlueprintReader::BindWidgetEvent(std::string_view assetPath,
	std::string_view widgetName, std::string_view eventName,
	std::string_view handlerFunction) {
	auto j = RunOp({"-Op=BindWidgetEvent",
					"-Asset="   + std::string(assetPath),
					"-Widget="  + std::string(widgetName),
					"-Event="   + std::string(eventName),
					"-Handler=" + std::string(handlerFunction)});
	BindWidgetEventResult out;
	out.assetPath       = std::string(assetPath);
	out.widgetName      = std::string(widgetName);
	out.eventName       = std::string(eventName);
	out.handlerFunction = std::string(handlerFunction);
	if (j.is_object())
	{
		out.bound = j.value("bound", false);
	}
	return out;
}

IBlueprintReader::CompileWidgetBlueprintResult
SocketBlueprintReader::CompileWidgetBlueprint(std::string_view assetPath) {
	auto j = RunOp({"-Op=CompileWidgetBlueprint", "-Asset=" + std::string(assetPath)});
	CompileWidgetBlueprintResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

// ----- Behavior Tree authoring (Stage 2) ---------------------------------

std::vector<BPAssetSummary>
SocketBlueprintReader::ListBehaviorTrees(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListBehaviorTrees"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) {
			BPAssetSummary s; from_json(v, s); out.push_back(std::move(s));
		}
	}
	return out;
}

IBlueprintReader::BehaviorTreeInfo
SocketBlueprintReader::ReadBehaviorTree(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadBehaviorTree", "-Asset=" + std::string(assetPath)});
	BehaviorTreeInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	out.rootNodeId = j.value("root_node_id", std::string{});
	if (auto it = j.find("nodes"); it != j.end() && it->is_array()) {
		for (const auto& n : *it) {
			BTNode bn;
			bn.nodeId       = n.value("node_id",   std::string{});
			bn.className    = n.value("class",     std::string{});
			bn.nodeKind     = n.value("node_kind", std::string{});
			bn.parentNodeId = n.value("parent",    std::string{});
			out.nodes.push_back(std::move(bn));
		}
	}
	return out;
}

IBlueprintReader::AddBTNodeResult
SocketBlueprintReader::AddBTNode(std::string_view assetPath,
	std::string_view parentNodeId, std::string_view nodeKind,
	std::string_view nodeClass) {
	std::vector<std::string> args = {"-Op=AddBTNode",
									 "-Asset=" + std::string(assetPath),
									 "-Kind="  + std::string(nodeKind),
									 "-Class=" + std::string(nodeClass)};
	if (!parentNodeId.empty())
	{
		args.push_back("-Parent=" + std::string(parentNodeId));
	}
	auto j = RunOp(args);
	AddBTNodeResult out;
	out.assetPath = std::string(assetPath);
	out.className = std::string(nodeClass);
	out.nodeKind  = std::string(nodeKind);
	if (j.is_object()) out.nodeId = j.value("node_id", std::string{});
	return out;
}

IBlueprintReader::SetBTNodePropertyResult
SocketBlueprintReader::SetBTNodeProperty(std::string_view assetPath,
	std::string_view nodeId, std::string_view propertyName,
	std::string_view value) {
	auto j = RunOp({"-Op=SetBTNodeProperty",
					"-Asset="    + std::string(assetPath),
					"-Node="     + std::string(nodeId),
					"-Property=" + std::string(propertyName),
					"-Value="    + std::string(value)});
	SetBTNodePropertyResult out;
	out.assetPath    = std::string(assetPath);
	out.nodeId       = std::string(nodeId);
	out.propertyName = std::string(propertyName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

IBlueprintReader::CompileBehaviorTreeResult
SocketBlueprintReader::CompileBehaviorTree(std::string_view assetPath) {
	auto j = RunOp({"-Op=CompileBehaviorTree", "-Asset=" + std::string(assetPath)});
	CompileBehaviorTreeResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

// ----- DataAsset CRUD (Stage 2) ------------------------------------------

std::vector<BPAssetSummary>
SocketBlueprintReader::ListDataAssets(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListDataAssets"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) {
			BPAssetSummary s; from_json(v, s); out.push_back(std::move(s));
		}
	}
	return out;
}

IBlueprintReader::DataAssetInfo
SocketBlueprintReader::ReadDataAsset(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadDataAsset", "-Asset=" + std::string(assetPath)});
	DataAssetInfo out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.className  = j.value("class", std::string{});
		if (auto it = j.find("properties"); it != j.end())
		{
			out.properties = *it;
		}
	}
	return out;
}

IBlueprintReader::CreateDataAssetResult
SocketBlueprintReader::CreateDataAsset(std::string_view assetPath,
	std::string_view className) {
	auto j = RunOp({"-Op=CreateDataAsset",
					"-Asset=" + std::string(assetPath),
					"-Class=" + std::string(className)});
	CreateDataAssetResult out;
	out.assetPath = std::string(assetPath);
	out.className = std::string(className);
	if (j.is_object()) {
		out.created        = j.value("created", false);
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

IBlueprintReader::SetDataAssetPropertyResult
SocketBlueprintReader::SetDataAssetProperty(std::string_view assetPath,
	std::string_view propertyName, std::string_view value) {
	auto j = RunOp({"-Op=SetDataAssetProperty",
					"-Asset="    + std::string(assetPath),
					"-Property=" + std::string(propertyName),
					"-Value="    + std::string(value)});
	SetDataAssetPropertyResult out;
	out.assetPath    = std::string(assetPath);
	out.propertyName = std::string(propertyName);
	if (j.is_object()) {
		out.oldValue = j.value("old_value", std::string{});
		out.newValue = j.value("new_value", std::string{});
	}
	return out;
}

// ----- StateTree authoring (Stage 2) -------------------------------------

std::vector<BPAssetSummary>
SocketBlueprintReader::ListStateTrees(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListStateTrees"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) {
			BPAssetSummary s; from_json(v, s); out.push_back(std::move(s));
		}
	}
	return out;
}

IBlueprintReader::StateTreeInfo
SocketBlueprintReader::ReadStateTree(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadStateTree", "-Asset=" + std::string(assetPath)});
	StateTreeInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	if (auto it = j.find("states"); it != j.end() && it->is_array()) {
		for (const auto& s : *it) {
			StateTreeState st;
			st.stateId       = s.value("state_id", std::string{});
			st.name          = s.value("name",     std::string{});
			st.parentStateId = s.value("parent",   std::string{});
			out.states.push_back(std::move(st));
		}
	}
	if (auto it = j.find("transitions"); it != j.end() && it->is_array()) {
		for (const auto& t : *it) {
			StateTreeTransition tr;
			tr.fromStateId = t.value("from",    std::string{});
			tr.toStateId   = t.value("to",      std::string{});
			tr.trigger     = t.value("trigger", std::string{});
			out.transitions.push_back(std::move(tr));
		}
	}
	return out;
}

IBlueprintReader::AddStateTreeStateResult
SocketBlueprintReader::AddStateTreeState(std::string_view assetPath,
	std::string_view parentStateId, std::string_view name) {
	std::vector<std::string> args = {"-Op=AddStateTreeState",
									 "-Asset=" + std::string(assetPath),
									 "-Name="  + std::string(name)};
	if (!parentStateId.empty())
	{
		args.push_back("-Parent=" + std::string(parentStateId));
	}
	auto j = RunOp(args);
	AddStateTreeStateResult out;
	out.assetPath = std::string(assetPath);
	out.name      = std::string(name);
	if (j.is_object()) out.stateId = j.value("state_id", std::string{});
	return out;
}

IBlueprintReader::SetStateTreeTransitionResult
SocketBlueprintReader::SetStateTreeTransition(std::string_view assetPath,
	std::string_view fromStateId, std::string_view toStateId,
	std::string_view trigger) {
	auto j = RunOp({"-Op=SetStateTreeTransition",
					"-Asset="   + std::string(assetPath),
					"-From="    + std::string(fromStateId),
					"-To="      + std::string(toStateId),
					"-Trigger=" + std::string(trigger)});
	SetStateTreeTransitionResult out;
	out.assetPath   = std::string(assetPath);
	out.fromStateId = std::string(fromStateId);
	out.toStateId   = std::string(toStateId);
	out.trigger     = std::string(trigger);
	if (j.is_object())
	{
		out.added = j.value("added", false);
	}
	return out;
}

IBlueprintReader::CompileStateTreeResult
SocketBlueprintReader::CompileStateTree(std::string_view assetPath) {
	auto j = RunOp({"-Op=CompileStateTree", "-Asset=" + std::string(assetPath)});
	CompileStateTreeResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

// ----- Stage 3: profile / cook / class info / viewport ------------------

IBlueprintReader::StartProfileResult
SocketBlueprintReader::StartProfile(std::string_view mode) {
	auto j = RunOp({"-Op=StartProfile", "-Mode=" + std::string(mode)});
	StartProfileResult out;
	if (j.is_object()) {
		out.started    = j.value("started", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::StopProfileResult
SocketBlueprintReader::StopProfile() {
	auto j = RunOp({"-Op=StopProfile"});
	StopProfileResult out;
	if (j.is_object()) {
		out.stopped    = j.value("stopped", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::StatGroupResult
SocketBlueprintReader::GetStats(std::string_view group) {
	auto j = RunOp({"-Op=GetStats", "-Group=" + std::string(group)});
	StatGroupResult out;
	out.group = std::string(group);
	if (j.is_object()) out.snapshot = j.value("snapshot", std::string{});
	return out;
}

IBlueprintReader::ScreenshotResult
SocketBlueprintReader::TakeScreenshot(std::string_view destPath, int width, int height) {
	auto j = RunOp({"-Op=TakeScreenshot",
					"-Dest="   + std::string(destPath),
					"-Width="  + std::to_string(width),
					"-Height=" + std::to_string(height)});
	ScreenshotResult out;
	if (j.is_object()) {
		out.captured   = j.value("captured", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::CookResult
SocketBlueprintReader::CookContent(std::string_view platform) {
	auto j = RunOp({"-Op=CookContent", "-Platform=" + std::string(platform)});
	CookResult out;
	out.platform = std::string(platform);
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::CookResult
SocketBlueprintReader::PackageProject(std::string_view platform, std::string_view outputDir) {
	auto j = RunOp({"-Op=PackageProject",
					"-Platform=" + std::string(platform),
					"-Output="   + std::string(outputDir)});
	CookResult out;
	out.platform = std::string(platform);
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::ClassInfo
SocketBlueprintReader::IntrospectClass(std::string_view className) {
	auto j = RunOp({"-Op=IntrospectClass", "-Class=" + std::string(className)});
	ClassInfo out;
	if (!j.is_object())
	{
		return out;
	}
	out.className   = j.value("class",  std::string{});
	out.parentClass = j.value("parent", std::string{});
	if (auto it = j.find("ancestors"); it != j.end() && it->is_array()) {
		for (const auto& a : *it)
		{
			if (a.is_string()) out.ancestors.push_back(a.get<std::string>());
		}
	}
	if (auto it = j.find("properties"); it != j.end() && it->is_array()) {
		for (const auto& p : *it) {
			ClassPropertyInfo cp;
			cp.name       = p.value("name",        std::string{});
			cp.typeName   = p.value("type",        std::string{});
			cp.category   = p.value("category",    std::string{});
			cp.declaredOn = p.value("declared_on", std::string{});
			out.properties.push_back(std::move(cp));
		}
	}
	if (auto it = j.find("functions"); it != j.end() && it->is_array()) {
		for (const auto& f : *it) {
			ClassFunctionInfo cf;
			cf.name       = f.value("name",        std::string{});
			cf.flagsCsv   = f.value("flags",       std::string{});
			cf.declaredOn = f.value("declared_on", std::string{});
			out.functions.push_back(std::move(cf));
		}
	}
	return out;
}

IBlueprintReader::FindClassResult
SocketBlueprintReader::FindClass(std::string_view query) {
	auto j = RunOp({"-Op=FindClass", "-Query=" + std::string(query)});
	FindClassResult out;
	if (j.is_object()) {
		if (auto it = j.find("classes"); it != j.end() && it->is_array()) {
			for (const auto& c : *it) {
				if (c.is_string())
				{
					out.classNames.push_back(c.get<std::string>());
				}
			}
		}
	}
	return out;
}

std::vector<IBlueprintReader::ClassFunctionInfo>
SocketBlueprintReader::ListFunctions(std::string_view className) {
	auto j = RunOp({"-Op=ListFunctions", "-Class=" + std::string(className)});
	std::vector<ClassFunctionInfo> out;
	if (j.is_array()) {
		for (const auto& f : j) {
			ClassFunctionInfo cf;
			cf.name     = f.value("name",  std::string{});
			cf.flagsCsv = f.value("flags", std::string{});
			out.push_back(std::move(cf));
		}
	}
	return out;
}

IBlueprintReader::FocusActorResult
SocketBlueprintReader::FocusActor(std::string_view actorName) {
	auto j = RunOp({"-Op=FocusActor", "-Actor=" + std::string(actorName)});
	FocusActorResult out;
	out.actorName = std::string(actorName);
	if (j.is_object())
	{
		out.focused = j.value("focused", false);
	}
	return out;
}

IBlueprintReader::SetCameraResult
SocketBlueprintReader::SetCameraTransform(double lx, double ly, double lz,
										double rp, double ry, double rr) {
	auto j = RunOp({"-Op=SetCameraTransform",
					"-LX=" + std::to_string(lx),
					"-LY=" + std::to_string(ly),
					"-LZ=" + std::to_string(lz),
					"-RP=" + std::to_string(rp),
					"-RY=" + std::to_string(ry),
					"-RR=" + std::to_string(rr)});
	SetCameraResult out;
	if (j.is_object())
	{
		out.moved = j.value("moved", false);
	}
	return out;
}

IBlueprintReader::ViewportScreenshotResult
SocketBlueprintReader::TakeViewportScreenshot(std::string_view destPath) {
	auto j = RunOp({"-Op=TakeViewportScreenshot", "-Dest=" + std::string(destPath)});
	ViewportScreenshotResult out;
	if (j.is_object()) {
		out.captured   = j.value("captured", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::SetShowFlagResult
SocketBlueprintReader::SetShowFlag(std::string_view flagName, bool enabled) {
	auto j = RunOp({"-Op=SetShowFlag",
					"-Flag=" + std::string(flagName),
					std::string("-Enabled=") + (enabled ? "1" : "0")});
	SetShowFlagResult out;
	out.flagName = std::string(flagName);
	if (j.is_object())
	{
		out.enabled = j.value("enabled", false);
	}
	return out;
}

// ----- Stage 4: Niagara / Sequencer / GAS / AnimGraph -------------------

std::vector<BPAssetSummary>
SocketBlueprintReader::ListNiagaraSystems(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListNiagaraSystems"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::NiagaraSystemInfo
SocketBlueprintReader::ReadNiagaraSystem(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadNiagaraSystem", "-Asset=" + std::string(assetPath)});
	NiagaraSystemInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	if (auto it = j.find("emitters"); it != j.end() && it->is_array()) {
		for (const auto& e : *it) {
			NiagaraEmitterHandleInfo h;
			h.name        = e.value("name", std::string{});
			h.emitterPath = e.value("emitter_path", std::string{});
			h.enabled     = e.value("enabled", false);
			out.emitters.push_back(std::move(h));
		}
	}
	if (auto it = j.find("parameter_names"); it != j.end() && it->is_array()) {
		for (const auto& v : *it)
		{
			if (v.is_string()) out.parameterNames.push_back(v.get<std::string>());
		}
	}
	return out;
}

IBlueprintReader::CreateNiagaraSystemResult
SocketBlueprintReader::CreateNiagaraSystem(std::string_view assetPath) {
	auto j = RunOp({"-Op=CreateNiagaraSystem", "-Asset=" + std::string(assetPath)});
	CreateNiagaraSystemResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.created        = j.value("created", false);
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

IBlueprintReader::SetNiagaraParameterResult
SocketBlueprintReader::SetNiagaraParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view value) {
	auto j = RunOp({"-Op=SetNiagaraParameter",
					"-Asset=" + std::string(assetPath),
					"-Param=" + std::string(parameterName),
					"-Value=" + std::string(value)});
	SetNiagaraParameterResult out;
	out.assetPath     = std::string(assetPath);
	out.parameterName = std::string(parameterName);
	out.newValue      = std::string(value);
	if (j.is_object())
	{
		out.applied = j.value("applied", false);
	}
	return out;
}

std::vector<BPAssetSummary>
SocketBlueprintReader::ListLevelSequences(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListLevelSequences"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::LevelSequenceInfo
SocketBlueprintReader::ReadLevelSequence(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadLevelSequence", "-Asset=" + std::string(assetPath)});
	LevelSequenceInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	out.startSeconds = j.value("start_seconds", 0.0);
	out.endSeconds   = j.value("end_seconds",   0.0);
	if (auto it = j.find("tracks"); it != j.end() && it->is_array()) {
		for (const auto& t : *it) {
			SequenceTrackInfo st;
			st.trackName    = t.value("name",          std::string{});
			st.trackClass   = t.value("class",         std::string{});
			st.sectionCount = t.value("section_count", 0);
			out.tracks.push_back(std::move(st));
		}
	}
	return out;
}

IBlueprintReader::AddSequenceTrackResult
SocketBlueprintReader::AddSequenceTrack(std::string_view assetPath,
	std::string_view trackClass, std::string_view trackName) {
	auto j = RunOp({"-Op=AddSequenceTrack",
					"-Asset=" + std::string(assetPath),
					"-Class=" + std::string(trackClass),
					"-Name="  + std::string(trackName)});
	AddSequenceTrackResult out;
	out.assetPath  = std::string(assetPath);
	out.trackName  = std::string(trackName);
	out.trackClass = std::string(trackClass);
	if (j.is_object())
	{
		out.added = j.value("added", false);
	}
	return out;
}

IBlueprintReader::SetSequencePlaybackRangeResult
SocketBlueprintReader::SetSequencePlaybackRange(std::string_view assetPath,
	double startSeconds, double endSeconds) {
	auto j = RunOp({"-Op=SetSequencePlaybackRange",
					"-Asset=" + std::string(assetPath),
					"-Start=" + std::to_string(startSeconds),
					"-End="   + std::to_string(endSeconds)});
	SetSequencePlaybackRangeResult out;
	out.assetPath    = std::string(assetPath);
	out.startSeconds = startSeconds;
	out.endSeconds   = endSeconds;
	if (j.is_object())
	{
		out.applied = j.value("applied", false);
	}
	return out;
}

IBlueprintReader::GameplayTagListResult
SocketBlueprintReader::ListGameplayTags(std::string_view filter) {
	std::vector<std::string> args = {"-Op=ListGameplayTags"};
	if (!filter.empty())
	{
		args.push_back("-Filter=" + std::string(filter));
	}
	auto j = RunOp(args);
	GameplayTagListResult out;
	if (j.is_object()) {
		if (auto it = j.find("tags"); it != j.end() && it->is_array()) {
			for (const auto& v : *it)
			{
				if (v.is_string()) out.tags.push_back(v.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::AddGameplayTagResult
SocketBlueprintReader::AddGameplayTag(std::string_view tagName,
	std::string_view comment) {
	std::vector<std::string> args = {"-Op=AddGameplayTag",
									 "-Tag=" + std::string(tagName)};
	if (!comment.empty())
	{
		args.push_back("-Comment=" + std::string(comment));
	}
	auto j = RunOp(args);
	AddGameplayTagResult out;
	out.tagName = std::string(tagName);
	if (j.is_object()) {
		out.added          = j.value("added", false);
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

IBlueprintReader::AbilitySetInfo
SocketBlueprintReader::ReadAbilitySet(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadAbilitySet", "-Asset=" + std::string(assetPath)});
	AbilitySetInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	if (auto it = j.find("abilities"); it != j.end() && it->is_array()) {
		for (const auto& a : *it) {
			AbilitySetEntry e;
			e.abilityClass = a.value("class", std::string{});
			e.level        = a.value("level", 1);
			out.abilities.push_back(std::move(e));
		}
	}
	return out;
}

std::vector<BPAssetSummary>
SocketBlueprintReader::ListAnimBlueprints(std::string_view path) {
	std::vector<std::string> args = {"-Op=ListAnimBlueprints"};
	if (!path.empty())
	{
		args.push_back("-Path=" + std::string(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::AnimBlueprintInfo
SocketBlueprintReader::ReadAnimBlueprint(std::string_view assetPath) {
	auto j = RunOp({"-Op=ReadAnimBlueprint", "-Asset=" + std::string(assetPath)});
	AnimBlueprintInfo out;
	out.assetPath = std::string(assetPath);
	if (!j.is_object())
	{
		return out;
	}
	out.parentClass = j.value("parent_class", std::string{});
	if (auto it = j.find("state_machines"); it != j.end() && it->is_array()) {
		for (const auto& sm : *it) {
			AnimStateMachineInfo asm_info;
			asm_info.name = sm.value("name", std::string{});
			if (auto sIt = sm.find("states"); sIt != sm.end() && sIt->is_array()) {
				for (const auto& s : *sIt) {
					AnimStateInfo si;
					si.name = s.value("name", std::string{});
					si.kind = s.value("kind", std::string{});
					asm_info.states.push_back(std::move(si));
				}
			}
			out.stateMachines.push_back(std::move(asm_info));
		}
	}
	return out;
}

IBlueprintReader::AddAnimStateResult
SocketBlueprintReader::AddAnimState(std::string_view assetPath,
	std::string_view stateMachine, std::string_view stateName) {
	auto j = RunOp({"-Op=AddAnimState",
					"-Asset="   + std::string(assetPath),
					"-Machine=" + std::string(stateMachine),
					"-Name="    + std::string(stateName)});
	AddAnimStateResult out;
	out.assetPath    = std::string(assetPath);
	out.stateMachine = std::string(stateMachine);
	out.stateName    = std::string(stateName);
	if (j.is_object())
	{
		out.added = j.value("added", false);
	}
	return out;
}

IBlueprintReader::CompileAnimBlueprintResult
SocketBlueprintReader::CompileAnimBlueprint(std::string_view assetPath) {
	auto j = RunOp({"-Op=CompileAnimBlueprint", "-Asset=" + std::string(assetPath)});
	CompileAnimBlueprintResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

void SocketBlueprintReader::BeginBatch() {
	(void)RunOp({"-Op=BeginBatch"});
}

nlohmann::json SocketBlueprintReader::EndBatch(bool skipCompile) {
	std::vector<std::string> args = {"-Op=EndBatch"};
	if (skipCompile)
	{
		args.push_back("-Skip");
	}
	return RunOp(args);
}

nlohmann::json SocketBlueprintReader::ShutdownDaemon() {
	// Live mode has no daemon to shut down — the editor runs
	// independently of the MCP server's lifetime. The right "shutdown"
	// semantics here would be "close the socket connection", which
	// happens automatically in the destructor anyway. Return a neutral
	// ack so callers don't see this as an error.
	return nlohmann::json{
		{"ok", true},
		{"was_running", false},
		{"hint", "Live backend has no daemon process; close the editor "
				 "to release project locks."},
	};
}

}    // namespace bpr::backends
