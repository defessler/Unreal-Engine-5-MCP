#include "backends/CommandletBlueprintReader.h"

#include "Env.h"
#include "backends/CommandletArgEncoding.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif    // WIN32_LEAN_AND_MEAN
	// winsock2.h must come before windows.h; otherwise it tries to pull
	// in winsock.h via windows.h and the symbols clash. Daemon-attach
	// path does an inline TCP probe before constructing the
	// SocketBlueprintReader.
	#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "Ws2_32.lib")
#else    // defined(_WIN32)
	#include <signal.h>
#endif    // defined(_WIN32)

namespace bpr::backends {

namespace commandlet_blueprint_reader_detail {

#if defined(_WIN32)

std::wstring Widen(std::string_view s) {
	if (s.empty())
	{
		return L"";
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring out(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
	return out;
}

std::string Narrow(const std::wstring& s) {
	if (s.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
								nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
						out.data(), n, nullptr, nullptr);
	return out;
}

// CommandLineToArgvW round-trip safe quoting per Microsoft's parsing rules.
std::wstring QuoteArg(std::wstring_view in) {
	if (!in.empty() && in.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(in);
	}
	std::wstring out;
	out.reserve(in.size() + 2);
	out.push_back(L'"');
	for (size_t i = 0; i < in.size();) {
		size_t backslashes = 0;
		while (i < in.size() && in[i] == L'\\') { ++backslashes; ++i; }
		if (i == in.size()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (in[i] == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
			++i;
		} else {
			out.append(backslashes, L'\\');
			out.push_back(in[i]);
			++i;
		}
	}
	out.push_back(L'"');
	return out;
}

// Encoders live in CommandletArgEncoding.h so they're unit-testable
// without bringing in UE. EncodeArg dispatches per-arg between
// FParse-style inner quoting (for -Key=Value) and Windows outer
// quoting (for positional args like the uproject path).
using bpr::backends::detail::EncodeArg;
// EncodeArgForFParse used to be consumed by the stdin/stdout daemon
// path's inline arg join. The daemon now receives args structured (one
// per JSON-array element) over TCP, so the inner-FParse quoting layer
// belongs on the daemon side, not the client side. RunOpOneShot still
// uses EncodeArg via BuildCommandLine.

std::wstring BuildCommandLine(const std::wstring& exe,
							  const std::vector<std::wstring>& args) {
	std::wstring cmd = QuoteArg(exe);
	for (const auto& a : args) {
		cmd.push_back(L' ');
		cmd.append(EncodeArg(a));
	}
	return cmd;
}

// Split a whitespace-separated string into tokens. No quote handling — UE
// commandlet args almost never contain spaces in their values, so anything
// fancier than std::istringstream is overkill for now.
std::vector<std::wstring> SplitArgs(const std::string& s) {
	std::vector<std::wstring> out;
	std::istringstream iss(s);
	std::string tok;
	while (iss >> tok)
	{
		out.push_back(Widen(tok));
	}
	return out;
}

struct ProcResult {
	bool launched = false;
	bool timedOut = false;
	DWORD exitCode = 0;
	std::string stdoutTail;
	std::string stderrTail;
	std::string failureReason;
};

// Append `n` bytes from `buf` to `tail`, capping `tail` at `cap` bytes by
// dropping from the front. Cuts at the next '\n' boundary so we don't leave
// a half-codepoint on UTF-8 path strings (UE log lines often contain non-
// ASCII content). Falls back to a hard erase if no newline is found within
// the cap window.
void AppendTail(std::string& tail, const char* buf, size_t n, size_t cap = 8192) {
	tail.append(buf, n);
	if (tail.size() > cap) {
		std::size_t cutFrom = tail.size() - cap;
		auto nl = tail.find('\n', cutFrom);
		if (nl != std::string::npos) {
			tail.erase(0, nl + 1);
		} else {
			tail.erase(0, cutFrom);
		}
	}
}

// Tiny RAII wrapper around a Win32 HANDLE. CloseHandle is safe on nullptr
// and on INVALID_HANDLE_VALUE — but we keep our own nullptr-guard so the
// destructor doesn't loudly fail on Windows builds with stricter flags.
struct UniqueHandle {
	HANDLE h = nullptr;
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE x) : h(x) {}
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	UniqueHandle(UniqueHandle&& other) noexcept : h(other.h) { other.h = nullptr; }
	UniqueHandle& operator=(UniqueHandle&& other) noexcept {
		if (this != &other) { Reset(); h = other.h; other.h = nullptr; }
		return *this;
	}
	~UniqueHandle() { Reset(); }
	void Reset(HANDLE x = nullptr) {
		if (h && h != INVALID_HANDLE_VALUE)
		{
			CloseHandle(h);
		}
		h = x;
	}
	HANDLE Release() { HANDLE x = h; h = nullptr; return x; }
	explicit operator bool() const { return h != nullptr && h != INVALID_HANDLE_VALUE; }
};

// Generic scope guard: invoke a callable when the scope exits, unless
// Dismiss() was called. Used to clean up the per-call temp file even if
// any code between RunOpOneShot's "create file" and "drop file" throws,
// plus a couple of inline winsock cleanup paths in
// TryAttachExistingDaemon.
template <typename F>
class ScopeGuard {
public:
	explicit ScopeGuard(F f) : f_(std::move(f)) {}
	~ScopeGuard() { if (active_) f_(); }
	void Dismiss() { active_ = false; }
	ScopeGuard(const ScopeGuard&) = delete;
	ScopeGuard& operator=(const ScopeGuard&) = delete;
private:
	F f_;
	bool active_ = true;
};
template <typename F> ScopeGuard<F> MakeScopeGuard(F f) { return ScopeGuard<F>(std::move(f)); }

ProcResult RunChild(const std::wstring& exe,
					const std::vector<std::wstring>& args,
					std::chrono::seconds timeout) {
	ProcResult res;

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE outR = nullptr, outW = nullptr;
	HANDLE errR = nullptr, errW = nullptr;
	if (!CreatePipe(&outR, &outW, &sa, 0)) {
		res.failureReason = "CreatePipe(stdout) failed";
		return res;
	}
	if (!CreatePipe(&errR, &errW, &sa, 0)) {
		CloseHandle(outR); CloseHandle(outW);
		res.failureReason = "CreatePipe(stderr) failed";
		return res;
	}
	SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

	std::wstring cmd = BuildCommandLine(exe, args);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = outW;
	si.hStdError = errW;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(
		exe.c_str(),
		cmd.data(),
		nullptr, nullptr,
		TRUE,
		CREATE_NO_WINDOW,
		nullptr,
		nullptr,
		&si,
		&pi);
	CloseHandle(outW);
	CloseHandle(errW);
	if (!ok) {
		DWORD err = GetLastError();
		res.failureReason = fmt::format("CreateProcessW failed (err={})", err);
		CloseHandle(outR);
		CloseHandle(errR);
		return res;
	}
	res.launched = true;

	auto deadline = std::chrono::steady_clock::now() + timeout;

	auto drain = [](HANDLE h, std::string& tail) {
		for (;;) {
			DWORD avail = 0;
			if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
			{
				return;
			}
			if (avail == 0)
			{
				return;
			}
			char buf[1024];
			DWORD got = 0;
			DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
			if (!ReadFile(h, buf, toRead, &got, nullptr) || got == 0)
			{
				return;
			}
			AppendTail(tail, buf, got);
		}
	};

	for (;;) {
		DWORD waitMs = 100;
		DWORD wr = WaitForSingleObject(pi.hProcess, waitMs);
		drain(outR, res.stdoutTail);
		drain(errR, res.stderrTail);
		if (wr == WAIT_OBJECT_0)
		{
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			TerminateProcess(pi.hProcess, 9);
			WaitForSingleObject(pi.hProcess, 2000);
			res.timedOut = true;
			res.failureReason = "timeout";
			break;
		}
	}
	// Final drain after the wait loop exits — anything written between the
	// last in-loop drain and process exit/terminate would otherwise be lost
	// from res.stderrTail, which is exactly what shows up in our error
	// messages.
	drain(outR, res.stdoutTail);
	drain(errR, res.stderrTail);

	// Initialize code explicitly. If GetExitCodeProcess fails we want a
	// sentinel rather than whatever uninit value it left behind.
	DWORD code = 0xFFFFFFFF;
	if (!GetExitCodeProcess(pi.hProcess, &code)) {
		code = 0xFFFFFFFF;
	}
	res.exitCode = code;

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(outR);
	CloseHandle(errR);
	return res;
}

#else    // !_WIN32

std::wstring Widen(std::string_view) { return L""; }
std::string Narrow(const std::wstring&) { return ""; }

struct ProcResult {
	bool launched = false;
	bool timedOut = false;
	int exitCode = 0;
	std::string stdoutTail;
	std::string stderrTail;
	std::string failureReason;
};

ProcResult RunChild(const std::wstring&, const std::vector<std::wstring>&, std::chrono::seconds) {
	ProcResult r;
	r.failureReason = "CommandletBlueprintReader is Windows-only.";
	return r;
}

#endif    // defined(_WIN32)

std::string TrimLines(const std::string& s, std::size_t maxLines) {
	if (s.empty())
	{
		return s;
	}
	std::deque<std::string> lines;
	std::size_t start = 0;
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\n') {
			lines.emplace_back(s.substr(start, i - start));
			start = i + 1;
			if (lines.size() > maxLines)
			{
				lines.pop_front();
			}
		}
	}
	if (start < s.size()) {
		lines.emplace_back(s.substr(start));
		if (lines.size() > maxLines)
		{
			lines.pop_front();
		}
	}
	std::string out;
	for (const auto& l : lines) {
		out.append(l).push_back('\n');
	}
	return out;
}

// FNV-1a 64-bit hash of an arbitrary string. Used to prefix temp-dir
// scratch files with a project-derived tag so a glance at the temp
// directory shows which project owns which files when multiple UE
// projects are active.
uint64_t FnvHash64(std::string_view s) {
	uint64_t h = 0xcbf29ce484222325ull;
	for (char c : s) {
		h ^= static_cast<uint8_t>(c);
		h *= 0x100000001b3ull;
	}
	return h;
}

std::filesystem::path TempJsonPath(std::string_view projectTag) {
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, buf);
	std::filesystem::path tmp = (n == 0) ? std::filesystem::path(L"C:\\Windows\\Temp")
										 : std::filesystem::path(std::wstring(buf, n));
#else    // defined(_WIN32)
	std::filesystem::path tmp = std::filesystem::temp_directory_path();
#endif    // defined(_WIN32)
	char prefix[32];
	std::snprintf(prefix, sizeof(prefix), "%016llx",
		static_cast<unsigned long long>(FnvHash64(projectTag)));
	std::random_device rd;
	std::mt19937_64 rng(rd());
	std::uniform_int_distribution<uint64_t> dist;
	std::ostringstream name;
	name << "bp-reader-" << prefix << "-" << std::hex << dist(rng) << ".json";
	return tmp / name.str();
}

std::string SecondsFromEnv(const char* key, std::string fallback) {
#if defined(_MSC_VER)
	char* buf = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
		std::string out(buf);
		std::free(buf);
		return out.empty() ? fallback : out;
	}
	return fallback;
#else    // defined(_MSC_VER)
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0')
	{
		return std::string(v);
	}
	return fallback;
#endif    // defined(_MSC_VER)
}

// RAII exclusive file lock used to coordinate daemon spawn attempts
// across MCP-server processes. Held only while one MCP server is
// trying to bring up a daemon; OS auto-releases on process exit so a
// crashed spawner doesn't permanently block other MCP servers.
//
// Windows: CreateFileW with dwShareMode=0 (same idiom the daemon uses
// for its lifetime lock at Saved/bp-reader-cmdlet.lock — see the
// editor side's AcquireLifetimeLock). FILE_FLAG_DELETE_ON_CLOSE makes
// the lock file vanish when we release. POSIX: stub for now (daemon
// ships Windows-only today; cross-platform path would use ::open +
// ::flock(LOCK_EX|LOCK_NB)).
class SpawnLock {
public:
	explicit SpawnLock(std::filesystem::path lockFile)
		: lockFile_(std::move(lockFile)) {}
	~SpawnLock() { Release(); }
	SpawnLock(const SpawnLock&) = delete;
	SpawnLock& operator=(const SpawnLock&) = delete;

	// Try to acquire the exclusive lock, polling until either acquired
	// or `blockFor` elapses. Returns true on success.
	bool TryAcquire(std::chrono::seconds blockFor);

	bool IsHeld() const { return held_; }
	void Release();

private:
	std::filesystem::path lockFile_;
	bool held_ = false;
#if defined(_WIN32)
	void* handle_ = nullptr;  // HANDLE, opaque-typed to avoid leaking windows.h
#endif    // defined(_WIN32)
};

bool SpawnLock::TryAcquire(std::chrono::seconds blockFor) {
#if defined(_WIN32)
	// Make sure the parent directory exists; CreateFileW won't create
	// intermediate directories and a missing Saved/ folder would map
	// to ERROR_PATH_NOT_FOUND (not the "contended" error we'd retry on).
	std::error_code mkec;
	std::filesystem::create_directories(lockFile_.parent_path(), mkec);

	const auto deadline = std::chrono::steady_clock::now() + blockFor;
	while (true) {
		std::wstring wpath = lockFile_.wstring();
		HANDLE h = ::CreateFileW(wpath.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,                  // no sharing — exclusive
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
			nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			handle_ = h;
			held_ = true;
			return true;
		}
		const DWORD err = ::GetLastError();
		if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
			return false;  // unexpected error — don't loop on it
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
#else    // defined(_WIN32)
	// POSIX stub — flock-based version is a follow-up.
	(void)blockFor;
	return true;  // pretend acquired
#endif    // defined(_WIN32)
}

void SpawnLock::Release() {
	if (!held_)
	{
		return;
	}
#if defined(_WIN32)
	if (handle_) {
		::CloseHandle(static_cast<HANDLE>(handle_));
		handle_ = nullptr;
	}
#endif    // defined(_WIN32)
	held_ = false;
}

}    // namespace commandlet_blueprint_reader_detail
using namespace commandlet_blueprint_reader_detail;

CommandletBlueprintReader::CommandletBlueprintReader(Config cfg)
	: cfg_(std::move(cfg)) {
	if (cfg_.engineDir.empty()) {
		throw BlueprintReaderError("BP_READER_ENGINE_DIR is not set");
	}
	if (cfg_.uproject.empty()) {
		throw BlueprintReaderError("BP_READER_PROJECT is not set");
	}

	// UE binary naming for the editor commandlet exe (per
	// UEBuildBinary.cs::GetAdditionalConsoleAppPath): take the already
	// config-decorated binary name, strip the extension, append "-Cmd",
	// re-add the extension. So:
	//   Development: UnrealEditor.exe          -> UnrealEditor-Cmd.exe
	//   DebugGame:   UnrealEditor-Win64-DebugGame.exe
	//                                          -> UnrealEditor-Win64-DebugGame-Cmd.exe
	// The daemon must launch the variant whose config matches the
	// BlueprintReaderEditor module's compiled config — mismatch = UE
	// silently skips loading our plugin's DLL.
	{
		const std::string& cfgName = cfg_.editorConfig;
		std::filesystem::path candidate;
		const char* envCmd = std::getenv("BP_READER_EDITOR_CMD");
		if (envCmd != nullptr && envCmd[0] != '\0' &&
		    std::filesystem::exists(envCmd)) {
			candidate = std::filesystem::path(envCmd);
		} else {
			const char* envTarget = std::getenv("BP_READER_EDITOR_TARGET");
			if (envTarget != nullptr && envTarget[0] != '\0') {
				const auto projectBin = cfg_.uproject.parent_path() /
				                        "Binaries" / "Win64";
				auto projectCandidate = projectBin /
					fmt::format("{}-Cmd.exe", envTarget);
				if (std::filesystem::exists(projectCandidate)) {
					candidate = projectCandidate;
				}
			}
			if (candidate.empty()) {
				const auto binDir = cfg_.engineDir / "Engine" / "Binaries" / "Win64";
				candidate = (cfgName.empty() || cfgName == "Development")
					? binDir / "UnrealEditor-Cmd.exe"
					: binDir / fmt::format("UnrealEditor-Win64-{}-Cmd.exe", cfgName);
			}
		}
		if (!std::filesystem::exists(candidate)) {
			throw BlueprintReaderError(fmt::format(
				"editor -Cmd binary not found at: {}. Set BP_READER_EDITOR_CMD "
				"or BP_READER_EDITOR_TARGET.",
				candidate.string()));
		}
		editorCmdExe_ = candidate;
	}
	if (!std::filesystem::exists(cfg_.uproject)) {
		throw BlueprintReaderError(fmt::format(
			"uproject not found at: {}", cfg_.uproject.string()));
	}
}

CommandletBlueprintReader::~CommandletBlueprintReader() {
	// Join the prewarm thread first if it's still running.
	// EnsureDaemonAttached holds daemonMutex_, so by the time join()
	// returns either the daemon is attached or its setup failed and the
	// mutex is released. Either way we're safe to terminate.
	if (prewarmThread_.joinable()) {
		prewarmThread_.join();
	}
	// Drop the socket before tearing the child down — its destructor
	// closes the connection cleanly. Terminate the spawned child
	// afterward; if we never spawned (we attached to a daemon owned by
	// someone else) this is a no-op.
	socket_.reset();
#if defined(_WIN32)
	TerminateDaemon();
#endif    // defined(_WIN32)
}

namespace commandlet_blueprint_reader_detail2 {

// Convert an op-args vector of wstrings (the existing internal format
// used by every typed method on this class) into UTF-8 strings the
// socket reader's `RunOpRaw` accepts. The daemon's TCP server runs
// FParse against the joined arg string, just like the in-process
// commandlet, so we don't need to strip Windows-style outer quoting
// here — the args arrive structured (one per element).
std::vector<std::string> ToUtf8Args(const std::vector<std::wstring>& w) {
	std::vector<std::string> out;
	out.reserve(w.size());
	for (const auto& s : w)
	{
		out.push_back(Narrow(s));
	}
	return out;
}

// Win32 PID-aliveness probe. Used by TryAttachExistingDaemon to drop
// stale handshake files that survive a daemon crash. Returns false on
// any failure so a missing/uncertain answer is treated as "dead."
bool ProcessAlive(int pid) {
#if defined(_WIN32)
	if (pid <= 0)
	{
		return false;
	}
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
						   static_cast<DWORD>(pid));
	if (!h)
	{
		return false;
	}
	DWORD code = 0;
	BOOL ok = GetExitCodeProcess(h, &code);
	CloseHandle(h);
	return ok && code == STILL_ACTIVE;
#else    // defined(_WIN32)
	if (pid <= 0)
	{
		return false;
	}
	return ::kill(pid, 0) == 0;
#endif    // defined(_WIN32)
}

}    // namespace commandlet_blueprint_reader_detail2
using namespace commandlet_blueprint_reader_detail2;

nlohmann::json CommandletBlueprintReader::RunOp(const std::vector<std::wstring>& opArgs) {
	if (cfg_.useDaemon) {
		try {
			auto args = ToUtf8Args(opArgs);
			return EnsureDaemonAttached().RunOpRaw(args);
		} catch (const AssetNotFound&) {
			throw;  // user-level error; propagate as-is
		} catch (const std::exception& e) {
			// Daemon transport failure — log and fall through to one-shot.
			// Drop the stale socket and any child we spawned; the next
			// call's EnsureDaemonAttached spins up a fresh one.
			std::fprintf(stderr,
				"[bp-reader-mcp][commandlet][daemon] transport error, falling back to one-shot: %s\n",
				e.what());
			{
				std::lock_guard<std::mutex> lock(daemonMutex_);
				socket_.reset();
			}
#if defined(_WIN32)
			TerminateDaemon();
#endif    // defined(_WIN32)
			return RunOpOneShot(opArgs);
		}
	}
	return RunOpOneShot(opArgs);
}

nlohmann::json CommandletBlueprintReader::RunOpOneShot(const std::vector<std::wstring>& opArgs) {
	auto outFile = TempJsonPath(cfg_.uproject.string());

	std::vector<std::wstring> args;
	args.reserve(opArgs.size() + 8);
	args.push_back(cfg_.uproject.wstring());
	args.push_back(L"-run=BPR");
	for (const auto& a : opArgs) {
		args.push_back(a);
	}
	args.push_back(L"-Out=" + outFile.wstring());
	args.push_back(L"-Compact");
	args.push_back(L"-nullrhi");
	args.push_back(L"-nosplash");
	args.push_back(L"-unattended");
	args.push_back(L"-nopause");
	args.push_back(L"-stdout");
	for (auto& extra : SplitArgs(cfg_.editorExtraArgs))
	{
		args.push_back(std::move(extra));
	}

	const auto t0 = std::chrono::steady_clock::now();
	auto r = RunChild(editorCmdExe_.wstring(), args, cfg_.timeout);
	const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();

	if (env::VerboseLoggingEnabled()) {
		std::fprintf(stderr,
					 "[bp-reader-mcp][commandlet] op-args=%zu exit=%lu timed_out=%d duration=%lldms\n",
					 opArgs.size(), static_cast<unsigned long>(r.exitCode),
					 r.timedOut ? 1 : 0, static_cast<long long>(dt));
	}

	auto cleanup = [&]() {
		std::error_code ec;
		std::filesystem::remove(outFile, ec);
	};

	if (!r.launched) {
		cleanup();
		throw BlueprintReaderError(fmt::format(
			"failed to launch UnrealEditor-Cmd.exe: {}", r.failureReason));
	}
	if (r.timedOut) {
		cleanup();
		throw BlueprintReaderError(fmt::format(
			"commandlet timed out after {}s; tail of stderr:\n{}",
			cfg_.timeout.count(), TrimLines(r.stderrTail, 250)));
	}
	if (r.exitCode != 0) {
		std::string tail = TrimLines(r.stderrTail.empty() ? r.stdoutTail : r.stderrTail, 250);
		cleanup();
		if (r.exitCode == 4) {
			throw AssetNotFound(fmt::format(
				"commandlet reported missing target (exit=4); tail:\n{}", tail));
		}
		throw BlueprintReaderError(fmt::format(
			"commandlet exit={}; tail:\n{}", r.exitCode, tail));
	}

	if (!std::filesystem::exists(outFile)) {
		throw BlueprintReaderError(fmt::format(
			"commandlet exited 0 but produced no output file at {}", outFile.string()));
	}

	nlohmann::json parsed;
	try {
		std::ifstream in(outFile);
		in >> parsed;
	} catch (const std::exception& e) {
		cleanup();
		throw BlueprintReaderError(fmt::format(
			"failed to parse commandlet JSON ({}): {}", outFile.string(), e.what()));
	}
	cleanup();
	return parsed;
}

std::unique_ptr<SocketBlueprintReader>
CommandletBlueprintReader::TryAttachExistingDaemon() const {
	auto hsPath = cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet.json";
	std::error_code ec;
	if (!std::filesystem::exists(hsPath, ec))
	{
		return nullptr;
	}

	std::ifstream f(hsPath);
	if (!f)
	{
		return nullptr;
	}
	nlohmann::json j;
	try { f >> j; }
	catch (...) { return nullptr; }

	int pid = j.value("pid", 0);
	// pid is diagnostic-only on the editor side (lifetime lock is the
	// source of truth) but extremely useful here as a cheap "is the
	// daemon still alive?" probe before we sink a TCP connect.
	if (pid > 0 && !ProcessAlive(pid))
	{
		return nullptr;
	}

	SocketBlueprintReader::Config sc;
	sc.host  = j.value("host",  std::string("127.0.0.1"));
	sc.port  = j.value("port",  0);
	sc.token = j.value("token", std::string());
	if (sc.port <= 0 || sc.token.empty())
	{
		return nullptr;
	}

	// Wire the handshake-file path through to the socket reader so it
	// can self-refresh on connect-refused / auth-fail (issue #9 pattern
	// — daemon restart with a new port or token).
	sc.handshakeFilePath = hsPath.string();
	if (!cfg_.uproject.empty()) {
		sc.projectPath = cfg_.uproject.string();
	}
	sc.connectTimeout    = std::chrono::seconds(5);
	sc.opTimeout         = cfg_.timeout;

	// TCP probe with a short timeout. AutoBlueprintReader's TcpProbe is
	// not visible from here (TU-private), so do a quick non-blocking
	// connect inline. Skip the probe when we don't have a sensible
	// address — we'll just rely on EnsureConnected throwing on first op.
#if defined(_WIN32)
	{
		WSADATA wsa;
		bool wsaInited = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
		auto wsaCleanup = MakeScopeGuard([wsaInited]() {
			if (wsaInited)
			{
				WSACleanup();
			}
		});
		SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
		{
			return nullptr;
		}
		auto closeSock = MakeScopeGuard([s]() { ::closesocket(s); });
		u_long nb = 1; ::ioctlsocket(s, FIONBIO, &nb);

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = htons(static_cast<uint16_t>(sc.port));
		::inet_pton(AF_INET, sc.host.c_str(), &addr.sin_addr);

		int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		if (rc != 0) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(s, &wfds);
			timeval tv{};
			tv.tv_sec  = 0;
			tv.tv_usec = 250 * 1000;
			rc = ::select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
			if (rc <= 0)
			{
				return nullptr;
			}
			int err = 0;
			int errLen = sizeof(err);
			::getsockopt(s, SOL_SOCKET, SO_ERROR,
						 reinterpret_cast<char*>(&err), &errLen);
			if (err != 0)
			{
				return nullptr;
			}
		}
	}
#endif    // defined(_WIN32)

	try {
		return std::make_unique<SocketBlueprintReader>(std::move(sc));
	} catch (...) {
		return nullptr;
	}
}

void CommandletBlueprintReader::PollForHandshake(std::chrono::seconds timeout) {
	auto hsPath = cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet.json";
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		std::error_code ec;
		if (std::filesystem::exists(hsPath, ec)) {
			std::ifstream f(hsPath);
			nlohmann::json j;
			bool parsed = false;
			try {
				f >> j;
				parsed = true;
			} catch (...) {
				parsed = false;
			}
			if (parsed && j.value("port", 0) > 0 &&
				!j.value("token", std::string()).empty()) {
				return;  // ready
			}
		}
#if defined(_WIN32)
		// Bail early if the child we spawned died before publishing —
		// attaching to a dead daemon would just produce a confusing
		// TCP error on the first op.
		if (daemonProcess_ != nullptr &&
			WaitForSingleObject(daemonProcess_, 0) == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(daemonProcess_, &code);
			throw BlueprintReaderError(fmt::format(
				"daemon child exited before publishing handshake "
				"(code={}); check {} for engine log tail",
				code,
				(std::filesystem::temp_directory_path() /
				 "bp-reader-mcp-daemon-failure.log").string()));
		}
#endif    // defined(_WIN32)
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	// Caller's TryAttachExistingDaemon will return nullptr and
	// EnsureDaemonAttached will turn that into a useful error.
}

SocketBlueprintReader&
CommandletBlueprintReader::EnsureDaemonAttached() {
	std::lock_guard<std::mutex> lock(daemonMutex_);
	if (socket_)
	{
		return *socket_;
	}

	// First: see if a daemon is already listening (could be one we
	// spawned earlier this session, or a separately-launched editor
	// running the daemon). The lifetime lock on the editor side makes
	// sure we won't see two of them at once.
	socket_ = TryAttachExistingDaemon();
	if (socket_)
	{
		return *socket_;
	}

	// No daemon found — race for the inter-process spawn lock to
	// coordinate with other MCP servers that might also be trying to
	// spawn. Two locks at play (per "Daemon lifecycle" in the
	// multi-session plan):
	//   * bp-reader-cmdlet.lock       — held by the daemon for its
	//                                   lifetime; "is daemon alive?"
	//   * bp-reader-cmdlet-spawn.lock — held by an MCP server during
	//                                   its spawn attempt only; lets a
	//                                   second arriver wait on the
	//                                   handshake rather than
	//                                   double-spawning.
	//
	// Decoupling "alive" from "starting up" is what closes the Phase 2
	// race: if two MCP servers arrive at the same instant, only the
	// spawn-lock winner calls CreateProcessW; the loser polls the
	// handshake file that the winner is about to publish.
	auto spawnLockPath = cfg_.uproject.parent_path() / "Saved" /
		"bp-reader-cmdlet-spawn.lock";
	SpawnLock spawnLock(spawnLockPath);
	const bool acquired = spawnLock.TryAcquire(cfg_.startupTimeout);

	if (acquired) {
		// Re-check inside the lock — someone else may have finished
		// their spawn during our (zero or non-zero) wait.
		socket_ = TryAttachExistingDaemon();
		if (!socket_) {
			// Production path uses the built-in Win32 SpawnDaemon. The
			// test hook (Config::spawnDaemonHook) lets a unit test
			// simulate spawn latency + handshake-file publication
			// without actually launching UnrealEditor-Cmd.exe.
			if (cfg_.spawnDaemonHook) {
				cfg_.spawnDaemonHook(cfg_.uproject);
			} else {
				SpawnDaemon();
			}
			PollForHandshake(cfg_.startupTimeout);
			socket_ = TryAttachExistingDaemon();
		}
		// SpawnLock destructor releases the lock when this scope
		// exits — release happens AFTER PollForHandshake returns, so a
		// late-arriving second MCP server sees the handshake file
		// already in place (it'll just attach in TryAttachExistingDaemon
		// and never even reach the spawn-lock contention path).
	} else {
		// Spawn-lock contended past our timeout. Another MCP server is
		// (or was) mid-spawn — give the handshake one more chance to
		// appear and attach if it did. PollForHandshake is safe to
		// call without holding the spawn lock: it only reads the
		// handshake file.
		PollForHandshake(cfg_.startupTimeout);
		socket_ = TryAttachExistingDaemon();
	}

	if (!socket_) {
		// SpawnDaemon (or the hook) launched something but the
		// handshake never landed, OR the contention timed out without a
		// handshake. Tear our child down (if any) so the user can retry
		// without an orphaned editor process.
#if defined(_WIN32)
		TerminateDaemon();
#endif    // defined(_WIN32)
		throw BlueprintReaderError(fmt::format(
			"commandlet daemon: spawn-lock {}, handshake never appeared "
			"within {}s (bump BP_READER_STARTUP_TIMEOUT_SECONDS for "
			"slower projects); check {} for engine log tail",
			acquired ? "acquired" : "contended",
			cfg_.startupTimeout.count(),
			(std::filesystem::temp_directory_path() /
			 "bp-reader-mcp-daemon-failure.log").string()));
	}
	return *socket_;
}

#if defined(_WIN32)

void CommandletBlueprintReader::TerminateDaemon() {
	if (daemonProcess_ == nullptr)
	{
		return;
	}
	// Force termination of the child we spawned. The daemon's own
	// TCP-shutdown path is not wired yet (Phase 4) — TerminateProcess
	// is a hammer but reliable, and the daemon's lifetime lock cleans
	// up on process exit.
	TerminateProcess(daemonProcess_, 0);
	WaitForSingleObject(daemonProcess_, 1000);
	CloseHandle(daemonProcess_);
	daemonProcess_ = nullptr;
}

void CommandletBlueprintReader::Prewarm() {
	if (!cfg_.useDaemon)
	{
		return;
	}
	if (prewarmThread_.joinable())
	{
		return;  // already prewarming
	}
	prewarmThread_ = std::thread([this]() {
		try {
			// EnsureDaemonAttached holds daemonMutex_; a real call
			// hitting EnsureDaemonAttached concurrently will block on
			// the same mutex and find a hot socket once we're done.
			(void)EnsureDaemonAttached();
			if (env::VerboseLoggingEnabled()) {
				std::fprintf(stderr,
					"[bp-reader-mcp][commandlet][daemon] prewarm complete\n");
			}
		} catch (const std::exception& e) {
			// Swallow: the next real tool call will retry under its own
			// lock. Logging only — never let the prewarm thread crash main.
			std::fprintf(stderr,
				"[bp-reader-mcp][commandlet][daemon] prewarm failed: %s "
				"(tool calls will retry)\n", e.what());
		}
	});
}

void CommandletBlueprintReader::SpawnDaemon() {
	if (daemonProcess_ != nullptr) {
		// Sanity: if the child died unexpectedly, recycle. Use a 0-timeout
		// wait rather than GetExitCodeProcess + STILL_ACTIVE comparison —
		// the latter is fooled if the child legitimately exits with code
		// 259 (== STILL_ACTIVE).
		if (WaitForSingleObject(daemonProcess_, 0) == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(daemonProcess_, &code);
			std::fprintf(stderr,
				"[bp-reader-mcp][commandlet][daemon] previously-spawned "
				"child exited with code %lu; respawning\n",
				static_cast<unsigned long>(code));
			CloseHandle(daemonProcess_);
			daemonProcess_ = nullptr;
		} else {
			// Child still alive; assume it's racing toward publishing
			// its handshake. Let PollForHandshake decide.
			return;
		}
	}

	std::vector<std::wstring> args;
	args.push_back(cfg_.uproject.wstring());
	args.push_back(L"-run=BPR");
	args.push_back(L"-Daemon");
	args.push_back(L"-nullrhi");
	args.push_back(L"-nosplash");
	args.push_back(L"-unattended");
	args.push_back(L"-nopause");
	args.push_back(L"-stdout");
	for (auto& extra : SplitArgs(cfg_.editorExtraArgs)) {
		args.push_back(std::move(extra));
	}
	std::wstring cmd = BuildCommandLine(editorCmdExe_.wstring(), args);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	// Detached I/O: the daemon owns its own stdout/stderr (engine log
	// file) and accepts ops over TCP. No pipes — no parent-side reads.
	// CREATE_NO_WINDOW keeps the editor invisible.
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(
		editorCmdExe_.wstring().c_str(),
		cmd.data(),
		nullptr, nullptr,
		/*bInheritHandles=*/FALSE,
		CREATE_NO_WINDOW,
		nullptr, nullptr,
		&si, &pi);
	if (!ok) {
		DWORD err = GetLastError();
		throw BlueprintReaderError(fmt::format(
			"CreateProcessW(daemon) failed (err={})", err));
	}
	CloseHandle(pi.hThread);
	daemonProcess_ = pi.hProcess;

	if (env::VerboseLoggingEnabled()) {
		std::fprintf(stderr,
			"[bp-reader-mcp][commandlet][daemon] spawned UnrealEditor-Cmd "
			"(pid=%lu); waiting for handshake at "
			"<Project>/Saved/bp-reader-cmdlet.json (timeout=%llds)\n",
			static_cast<unsigned long>(pi.dwProcessId),
			static_cast<long long>(cfg_.startupTimeout.count()));
	}
}

#else    // !_WIN32

void CommandletBlueprintReader::TerminateDaemon() {}

void CommandletBlueprintReader::Prewarm() {
	// No-op on non-Windows; daemon mode is unsupported there.
}

void CommandletBlueprintReader::SpawnDaemon() {
	throw BlueprintReaderError("daemon mode is Windows-only");
}

#endif    // defined(_WIN32)

std::vector<BPAssetSummary> CommandletBlueprintReader::ListBlueprints(std::string_view path) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=List");
	if (!path.empty()) {
		args.push_back(L"-Path=" + Widen(path));
	}
	auto j = RunOp(args);
	return j.get<std::vector<BPAssetSummary>>();
}

BPMetadata CommandletBlueprintReader::ReadBlueprint(std::string_view assetPath) {
	auto j = RunOp({
		L"-Op=Read",
		L"-Asset=" + Widen(assetPath),
	});
	return j.get<BPMetadata>();
}

BPGraph CommandletBlueprintReader::GetGraph(std::string_view assetPath, std::string_view graphName) {
	auto j = RunOp({
		L"-Op=Graph",
		L"-Asset=" + Widen(assetPath),
		L"-Graph=" + Widen(graphName),
	});
	return j.get<BPGraph>();
}

BPFunction CommandletBlueprintReader::GetFunction(std::string_view assetPath, std::string_view fnName) {
	auto j = RunOp({
		L"-Op=Function",
		L"-Asset=" + Widen(assetPath),
		L"-Function=" + Widen(fnName),
	});
	return j.get<BPFunction>();
}

std::vector<BPVariable> CommandletBlueprintReader::ListVariables(std::string_view assetPath) {
	auto j = RunOp({
		L"-Op=Variables",
		L"-Asset=" + Widen(assetPath),
	});
	return j.get<std::vector<BPVariable>>();
}

std::vector<BPComponent> CommandletBlueprintReader::GetComponents(std::string_view assetPath) {
	auto j = RunOp({
		L"-Op=Components",
		L"-Asset=" + Widen(assetPath),
	});
	return j.get<std::vector<BPComponent>>();
}

std::vector<BPNode> CommandletBlueprintReader::FindNode(std::string_view assetPath,
														std::string_view query,
														std::string_view kind) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=Find");
	args.push_back(L"-Asset=" + Widen(assetPath));
	// Only pass -Query/-Kind when non-empty. UE's FParse::Value treats a
	// bare -Query= as an empty-string value; combined with another flag
	// following it (e.g. -Kind=...), the parser can swallow the wrong
	// token. Keeping the args list tight side-steps that.
	if (!query.empty()) {
		args.push_back(L"-Query=" + Widen(query));
	}
	if (!kind.empty()) {
		args.push_back(L"-Kind=" + Widen(kind));
	}
	auto j = RunOp(args);
	return j.get<std::vector<BPNode>>();
}

void CommandletBlueprintReader::AddVariable(std::string_view assetPath,
											std::string_view name,
											const BPPinType& type,
											std::string_view defaultValue,
											std::string_view category,
											bool replicated, bool editable) {
	// Pass the BPPinType as individual flags instead of a JSON blob — FParse's
	// quote handling can't survive a value that itself contains double quotes.
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddVariable");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name=" + Widen(name));
	args.push_back(L"-TypeCategory=" + Widen(type.Category));
	if (type.SubCategory.has_value() && !type.SubCategory->empty()) {
		args.push_back(L"-TypeSubCategory=" + Widen(*type.SubCategory));
	}
	if (type.SubCategoryObject.has_value() && !type.SubCategoryObject->empty()) {
		args.push_back(L"-TypeSubCategoryObject=" + Widen(*type.SubCategoryObject));
	}
	if (type.IsArray)
	{
		args.push_back(L"-TypeIsArray");
	}
	if (type.IsSet)
	{
		args.push_back(L"-TypeIsSet");
	}
	if (type.IsMap)
	{
		args.push_back(L"-TypeIsMap");
	}
	if (!defaultValue.empty()) {
		args.push_back(L"-Default=" + Widen(defaultValue));
	}
	if (!category.empty()) {
		args.push_back(L"-Category=" + Widen(category));
	}
	if (replicated)
	{
		args.push_back(L"-Replicated");
	}
	if (editable)
	{
		args.push_back(L"-Editable");
	}
	(void)RunOp(args);  // ack JSON `{"ok":true}` — we don't surface it
}

void CommandletBlueprintReader::SetNodePosition(std::string_view assetPath,
												std::string_view graphName,
												std::string_view nodeId,
												int x, int y) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=SetNodePosition");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Graph=" + Widen(graphName));
	args.push_back(L"-Node="  + Widen(nodeId));
	args.push_back(L"-X=" + std::to_wstring(x));
	args.push_back(L"-Y=" + std::to_wstring(y));
	(void)RunOp(args);
}

void CommandletBlueprintReader::DeleteNode(std::string_view assetPath,
										   std::string_view graphName,
										   std::string_view nodeId) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=DeleteNode");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Graph=" + Widen(graphName));
	args.push_back(L"-Node="  + Widen(nodeId));
	(void)RunOp(args);
}

std::string CommandletBlueprintReader::AddNode(std::string_view assetPath,
											   std::string_view graphName,
											   std::string_view kind,
											   int x, int y,
											   const std::map<std::string, std::string, std::less<>>& extras) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddNode");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Graph=" + Widen(graphName));
	args.push_back(L"-Kind="  + Widen(kind));
	args.push_back(L"-X=" + std::to_wstring(x));
	args.push_back(L"-Y=" + std::to_wstring(y));
	for (const auto& [k, v] : extras) {
		if (v.empty())
		{
			continue;
		}
		args.push_back(L"-" + Widen(k) + L"=" + Widen(v));
	}
	auto j = RunOp(args);
	if (!j.is_object() || !j.contains("node_id") || !j["node_id"].is_string()) {
		throw BlueprintReaderError("AddNode: response missing node_id");
	}
	return j["node_id"].get<std::string>();
}

void CommandletBlueprintReader::WirePins(std::string_view assetPath,
										 std::string_view graphName,
										 std::string_view fromNodeId,
										 std::string_view fromPinSpec,
										 std::string_view toNodeId,
										 std::string_view toPinSpec) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=WirePins");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Graph=" + Widen(graphName));
	args.push_back(L"-FromNode=" + Widen(fromNodeId));
	args.push_back(L"-FromPin="  + Widen(fromPinSpec));
	args.push_back(L"-ToNode="   + Widen(toNodeId));
	args.push_back(L"-ToPin="    + Widen(toPinSpec));
	(void)RunOp(args);
}

void CommandletBlueprintReader::DeleteVariable(std::string_view assetPath,
											   std::string_view name) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=DeleteVariable");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	(void)RunOp(args);
}

void CommandletBlueprintReader::RenameVariable(std::string_view assetPath,
											   std::string_view oldName,
											   std::string_view newName) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=RenameVariable");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-OldName=" + Widen(oldName));
	args.push_back(L"-NewName=" + Widen(newName));
	(void)RunOp(args);
}

namespace commandlet_blueprint_reader_detail3 {
// Append pin-type flags (-TypeCategory, -TypeSubCategory, etc.) for a BPPinType.
void AppendPinTypeFlags(std::vector<std::wstring>& args, const BPPinType& type) {
	args.push_back(L"-TypeCategory=" + Widen(type.Category));
	if (type.SubCategory.has_value() && !type.SubCategory->empty()) {
		args.push_back(L"-TypeSubCategory=" + Widen(*type.SubCategory));
	}
	if (type.SubCategoryObject.has_value() && !type.SubCategoryObject->empty()) {
		args.push_back(L"-TypeSubCategoryObject=" + Widen(*type.SubCategoryObject));
	}
	if (type.IsArray)
	{
		args.push_back(L"-TypeIsArray");
	}
	if (type.IsSet)
	{
		args.push_back(L"-TypeIsSet");
	}
	if (type.IsMap)
	{
		args.push_back(L"-TypeIsMap");
	}
}
}    // namespace commandlet_blueprint_reader_detail3
using namespace commandlet_blueprint_reader_detail3;

IBlueprintReader::AddFunctionResult
CommandletBlueprintReader::AddFunction(std::string_view assetPath,
									   std::string_view name) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddFunction");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	auto j = RunOp(args);
	AddFunctionResult out;
	out.functionName = std::string(name);
	if (j.is_object()) {
		if (j.contains("function_name") && j["function_name"].is_string()) {
			out.functionName = j["function_name"].get<std::string>();
		}
		if (j.contains("entry_node_id") && j["entry_node_id"].is_string()) {
			out.entryNodeId = j["entry_node_id"].get<std::string>();
		}
	}
	return out;
}

void CommandletBlueprintReader::AddFunctionInput(std::string_view assetPath,
												 std::string_view functionName,
												 std::string_view paramName,
												 const BPPinType& type) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddFunctionInput");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Function=" + Widen(functionName));
	args.push_back(L"-Param=" + Widen(paramName));
	AppendPinTypeFlags(args, type);
	(void)RunOp(args);
}

void CommandletBlueprintReader::AddFunctionOutput(std::string_view assetPath,
												  std::string_view functionName,
												  std::string_view paramName,
												  const BPPinType& type) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddFunctionOutput");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Function=" + Widen(functionName));
	args.push_back(L"-Param=" + Widen(paramName));
	AppendPinTypeFlags(args, type);
	(void)RunOp(args);
}

void CommandletBlueprintReader::DeleteFunction(std::string_view assetPath,
											   std::string_view name) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=DeleteFunction");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	(void)RunOp(args);
}

void CommandletBlueprintReader::SetVariableDefault(std::string_view assetPath,
												   std::string_view name,
												   std::string_view newDefault) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=SetVariableDefault");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	if (!newDefault.empty()) {
		args.push_back(L"-Default=" + Widen(newDefault));
	}
	(void)RunOp(args);
}

IBlueprintReader::CreateBlueprintResult
CommandletBlueprintReader::CreateBlueprint(std::string_view assetPath,
										   std::string_view parentClass) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=CreateBlueprint");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-ParentClass=" + Widen(parentClass));
	auto result = RunOp(args);
	CreateBlueprintResult out;
	if (result.is_object()) {
		out.alreadyExisted = result.value("already_existed", false);
		out.parentClass    = result.value("parent_class",   std::string{});
	}
	return out;
}

void CommandletBlueprintReader::SetPinDefault(std::string_view assetPath,
											  std::string_view graphName,
											  std::string_view nodeId,
											  std::string_view pinSpec,
											  std::string_view value) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=SetPinDefault");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Graph=" + Widen(graphName));
	args.push_back(L"-Node="  + Widen(nodeId));
	args.push_back(L"-Pin="   + Widen(pinSpec));
	args.push_back(L"-Value=" + Widen(value));
	(void)RunOp(args);
}

void CommandletBlueprintReader::RetypeVariable(std::string_view assetPath,
											   std::string_view name,
											   const BPPinType& newType) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=RetypeVariable");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	args.push_back(L"-TypeCategory=" + Widen(newType.Category));
	if (newType.SubCategory)
	{
		args.push_back(L"-TypeSubCategory=" + Widen(*newType.SubCategory));
	}
	if (newType.SubCategoryObject)
	{
		args.push_back(L"-TypeSubCategoryObject=" + Widen(*newType.SubCategoryObject));
	}
	if (newType.IsArray)
	{
		args.push_back(L"-TypeIsArray");
	}
	if (newType.IsSet)
	{
		args.push_back(L"-TypeIsSet");
	}
	if (newType.IsMap)
	{
		args.push_back(L"-TypeIsMap");
	}
	(void)RunOp(args);
}

void CommandletBlueprintReader::SetVariableCategory(std::string_view assetPath,
													std::string_view name,
													std::string_view category) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=SetVariableCategory");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Name="  + Widen(name));
	if (!category.empty())
	{
		args.push_back(L"-Category=" + Widen(category));
	}
	(void)RunOp(args);
}

IBlueprintReader::WriteGeneratedSourceResult
CommandletBlueprintReader::WriteGeneratedSource(std::string_view destPath,
												std::string_view content,
												bool createDirs) {
	// The daemon line-protocol can't carry multi-line content as a CLI
	// arg. Write the content to a server-side temp file first, then
	// pass that path via -ContentFile=<path>. The plugin reads + writes
	// + deletes the temp.
	namespace fs = std::filesystem;
	fs::path tempDir = fs::temp_directory_path();
	fs::path contentTemp = tempDir /
		("bpr-write-content-" + std::to_string(static_cast<unsigned long long>(
			std::hash<std::string>{}(std::string(destPath)))) + ".txt");
	{
		std::ofstream f(contentTemp, std::ios::binary);
		f.write(content.data(), static_cast<std::streamsize>(content.size()));
	}

	std::vector<std::wstring> args;
	args.push_back(L"-Op=WriteGeneratedSource");
	args.push_back(L"-Path=" + Widen(destPath));
	args.push_back(L"-ContentFile=" + contentTemp.wstring());
	if (createDirs)
	{
		args.push_back(L"-CreateDirs");
	}
	auto j = RunOp(args);

	// Plugin should have deleted the temp; clean up just in case.
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
CommandletBlueprintReader::DuplicateBlueprint(std::string_view sourceAssetPath,
											  std::string_view destAssetPath) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=DuplicateBlueprint");
	args.push_back(L"-Asset=" + Widen(sourceAssetPath));
	args.push_back(L"-Dest="  + Widen(destAssetPath));
	auto j = RunOp(args);
	DuplicateBlueprintResult out;
	out.alreadyExisted   = j.is_object() && j.value("already_existed", false);
	out.sourceAssetPath  = std::string(sourceAssetPath);
	return out;
}

nlohmann::json CommandletBlueprintReader::StructuralDiff(
	std::string_view a, std::string_view b, const StructuralDiffOptions& opts) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=StructuralDiff");
	args.push_back(L"-A=" + Widen(a));
	args.push_back(L"-B=" + Widen(b));
	// Plugin default for ignoreNodePositions is true; only emit the flag
	// when the caller wants to opt out (-IgnoreNodePositions=0). The
	// flag-style boolean -IgnoreCommentNodes enables comment-node
	// skipping (default off).
	if (!opts.ignoreNodePositions) {
		args.push_back(L"-IgnoreNodePositions=0");
	}
	if (opts.ignoreCommentNodes) {
		args.push_back(L"-IgnoreCommentNodes");
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
CommandletBlueprintReader::ListAssets(std::string_view path, bool recursive) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=ListAssets");
	if (!path.empty()) {
		args.push_back(L"-Path=" + Widen(path));
	}
	if (!recursive) {
		args.push_back(L"-NonRecursive");
	}
	return ParseAssetRegistryRows(RunOp(args), "assets");
}

IBlueprintReader::AssetRegistryListResult
CommandletBlueprintReader::FindAsset(std::string_view query, std::string_view path) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=FindAsset");
	args.push_back(L"-Query=" + Widen(query));
	if (!path.empty()) {
		args.push_back(L"-Path=" + Widen(path));
	}
	return ParseAssetRegistryRows(RunOp(args), "matches");
}

// ----- Project + Content Browser ops -------------------------------------

IBlueprintReader::ProjectMetadata
CommandletBlueprintReader::GetProjectMetadata() {
	// Pure local read — no daemon round-trip. The .uproject is already
	// resolved in cfg_.
	ProjectMetadata out;
	if (cfg_.uproject.empty()) {
		throw BlueprintReaderError("BP_READER_PROJECT is not set");
	}
	out.projectPath = cfg_.uproject.string();
	// Derive name from the filename stem.
	out.projectName = cfg_.uproject.stem().string();

	std::ifstream f(cfg_.uproject);
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
CommandletBlueprintReader::SaveAll(bool dirtyOnly) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=SaveAll");
	if (!dirtyOnly)
	{
		args.push_back(L"-IncludeClean");
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
CommandletBlueprintReader::MoveAsset(std::string_view sourcePath,
									 std::string_view destPath) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=MoveAsset");
	args.push_back(L"-Asset=" + Widen(sourcePath));
	args.push_back(L"-Dest="  + Widen(destPath));
	auto j = RunOp(args);
	MoveAssetResult out;
	out.sourcePath = std::string(sourcePath);
	out.destPath   = std::string(destPath);
	if (j.is_object()) {
		out.redirectorsCreated = j.value("redirectors_created", 0);
	}
	return out;
}

IBlueprintReader::DeleteAssetResult
CommandletBlueprintReader::DeleteAsset(std::string_view assetPath, bool force) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=DeleteAsset");
	args.push_back(L"-Asset=" + Widen(assetPath));
	if (force)
	{
		args.push_back(L"-Force");
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
CommandletBlueprintReader::CreateFolder(std::string_view folderPath) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=CreateFolder");
	args.push_back(L"-Path=" + Widen(folderPath));
	auto j = RunOp(args);
	CreateFolderResult out;
	out.path = std::string(folderPath);
	if (j.is_object()) {
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListDataTables(std::string_view path) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=ListDataTables");
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
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
CommandletBlueprintReader::ReadDataTable(std::string_view assetPath) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=ReadDataTable");
	args.push_back(L"-Asset=" + Widen(assetPath));
	auto j = RunOp(args);
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
CommandletBlueprintReader::AddDataRow(std::string_view assetPath,
									  std::string_view rowName,
									  const nlohmann::json& values,
									  bool overwrite) {
	// Serialize the values object to a JSON string and pass via a
	// temp file — the daemon line-protocol can't carry multi-line
	// JSON inline.
	namespace fs = std::filesystem;
	fs::path tempDir = fs::temp_directory_path();
	fs::path valuesTemp = tempDir /
		("bpr-add-row-" + std::to_string(static_cast<unsigned long long>(
			std::hash<std::string>{}(std::string(assetPath) + ":" +
									  std::string(rowName)))) + ".json");
	{
		std::ofstream f(valuesTemp);
		f << values.dump();
	}
	std::vector<std::wstring> args;
	args.push_back(L"-Op=AddDataRow");
	args.push_back(L"-Asset=" + Widen(assetPath));
	args.push_back(L"-Row="   + Widen(rowName));
	args.push_back(L"-ValuesFile=" + valuesTemp.wstring());
	if (overwrite)
	{
		args.push_back(L"-Overwrite");
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
CommandletBlueprintReader::SetDataRowValue(std::string_view assetPath,
										   std::string_view rowName,
										   std::string_view fieldName,
										   std::string_view value) {
	auto j = RunOp({
		L"-Op=SetDataRowValue",
		L"-Asset=" + Widen(assetPath),
		L"-Row="   + Widen(rowName),
		L"-Field=" + Widen(fieldName),
		L"-Value=" + Widen(value),
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
CommandletBlueprintReader::AddComponent(std::string_view assetPath,
										std::string_view name,
										std::string_view componentClass,
										std::string_view parentName,
										std::string_view socket) {
	std::vector<std::wstring> args = {
		L"-Op=AddComponent",
		L"-Asset=" + Widen(assetPath),
		L"-Name="  + Widen(name),
		L"-Class=" + Widen(componentClass),
	};
	if (!parentName.empty())
	{
		args.push_back(L"-Parent=" + Widen(parentName));
	}
	if (!socket.empty())
	{
		args.push_back(L"-Socket=" + Widen(socket));
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
CommandletBlueprintReader::RemoveComponent(std::string_view assetPath,
										   std::string_view name) {
	auto j = RunOp({L"-Op=RemoveComponent",
					L"-Asset=" + Widen(assetPath),
					L"-Name="  + Widen(name)});
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
CommandletBlueprintReader::AttachComponent(std::string_view assetPath,
										   std::string_view name,
										   std::string_view newParentName,
										   std::string_view socket) {
	std::vector<std::wstring> args = {
		L"-Op=AttachComponent",
		L"-Asset=" + Widen(assetPath),
		L"-Name="  + Widen(name),
	};
	if (!newParentName.empty())
	{
		args.push_back(L"-NewParent=" + Widen(newParentName));
	}
	if (!socket.empty())
	{
		args.push_back(L"-Socket="    + Widen(socket));
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
CommandletBlueprintReader::SetComponentProperty(std::string_view assetPath,
												std::string_view componentName,
												std::string_view propertyName,
												std::string_view value) {
	auto j = RunOp({L"-Op=SetComponentProperty",
					L"-Asset="     + Widen(assetPath),
					L"-Component=" + Widen(componentName),
					L"-Property="  + Widen(propertyName),
					L"-Value="     + Widen(value)});
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

// ----- Material authoring ------------------------------------------------

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListMaterials(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListMaterials"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
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
CommandletBlueprintReader::ReadMaterial(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadMaterial", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddMaterialExpression(std::string_view assetPath,
	std::string_view expressionClass, int x, int y) {
	auto j = RunOp({L"-Op=AddMaterialExpression",
					L"-Asset=" + Widen(assetPath),
					L"-Class=" + Widen(expressionClass),
					L"-X=" + std::to_wstring(x),
					L"-Y=" + std::to_wstring(y)});
	AddMaterialExpressionResult out;
	out.assetPath = std::string(assetPath);
	out.className = std::string(expressionClass);
	if (j.is_object()) out.expressionId = j.value("expression_id", std::string{});
	return out;
}

IBlueprintReader::ConnectMaterialResult
CommandletBlueprintReader::ConnectMaterialExpressions(std::string_view assetPath,
	std::string_view fromNodeId, std::string_view fromPin,
	std::string_view toNodeId, std::string_view toPin) {
	auto j = RunOp({L"-Op=ConnectMaterialExpressions",
					L"-Asset=" + Widen(assetPath),
					L"-From="  + Widen(fromNodeId),
					L"-FromPin=" + Widen(fromPin),
					L"-To="    + Widen(toNodeId),
					L"-ToPin=" + Widen(toPin)});
	ConnectMaterialResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.connected = j.value("connected", false);
	}
	return out;
}

IBlueprintReader::SetMaterialParameterResult
CommandletBlueprintReader::SetMaterialParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view value) {
	auto j = RunOp({L"-Op=SetMaterialParameter",
					L"-Asset=" + Widen(assetPath),
					L"-Param=" + Widen(parameterName),
					L"-Value=" + Widen(value)});
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
CommandletBlueprintReader::SetMaterialInstanceParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view paramType,
	std::string_view value) {
	auto j = RunOp({L"-Op=SetMaterialInstanceParameter",
					L"-Asset=" + Widen(assetPath),
					L"-Param=" + Widen(parameterName),
					L"-Type="  + Widen(paramType),
					L"-Value=" + Widen(value)});
	SetMIParameterResult out;
	out.assetPath     = std::string(assetPath);
	out.parameterName = std::string(parameterName);
	out.paramType     = std::string(paramType);
	if (j.is_object()) out.newValue = j.value("new_value", std::string{});
	return out;
}

IBlueprintReader::CompileMaterialResult
CommandletBlueprintReader::CompileMaterial(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CompileMaterial", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::ReadWidgetBlueprint(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadWidgetBlueprint", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddWidget(std::string_view assetPath,
	std::string_view parentName, std::string_view widgetClass,
	std::string_view name) {
	std::vector<std::wstring> args = {L"-Op=AddWidget",
									   L"-Asset=" + Widen(assetPath),
									   L"-Name="  + Widen(name),
									   L"-Class=" + Widen(widgetClass)};
	if (!parentName.empty())
	{
		args.push_back(L"-Parent=" + Widen(parentName));
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
CommandletBlueprintReader::SetWidgetProperty(std::string_view assetPath,
	std::string_view widgetName, std::string_view propertyName,
	std::string_view value) {
	auto j = RunOp({L"-Op=SetWidgetProperty",
					L"-Asset="    + Widen(assetPath),
					L"-Widget="   + Widen(widgetName),
					L"-Property=" + Widen(propertyName),
					L"-Value="    + Widen(value)});
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
CommandletBlueprintReader::BindWidgetEvent(std::string_view assetPath,
	std::string_view widgetName, std::string_view eventName,
	std::string_view handlerFunction) {
	auto j = RunOp({L"-Op=BindWidgetEvent",
					L"-Asset="   + Widen(assetPath),
					L"-Widget="  + Widen(widgetName),
					L"-Event="   + Widen(eventName),
					L"-Handler=" + Widen(handlerFunction)});
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
CommandletBlueprintReader::CompileWidgetBlueprint(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CompileWidgetBlueprint", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::ListBehaviorTrees(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListBehaviorTrees"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
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
CommandletBlueprintReader::ReadBehaviorTree(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadBehaviorTree", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddBTNode(std::string_view assetPath,
	std::string_view parentNodeId, std::string_view nodeKind,
	std::string_view nodeClass) {
	std::vector<std::wstring> args = {L"-Op=AddBTNode",
		L"-Asset=" + Widen(assetPath),
		L"-Kind="  + Widen(nodeKind),
		L"-Class=" + Widen(nodeClass)};
	if (!parentNodeId.empty())
	{
		args.push_back(L"-Parent=" + Widen(parentNodeId));
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
CommandletBlueprintReader::SetBTNodeProperty(std::string_view assetPath,
	std::string_view nodeId, std::string_view propertyName,
	std::string_view value) {
	auto j = RunOp({L"-Op=SetBTNodeProperty",
					L"-Asset="    + Widen(assetPath),
					L"-Node="     + Widen(nodeId),
					L"-Property=" + Widen(propertyName),
					L"-Value="    + Widen(value)});
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
CommandletBlueprintReader::CompileBehaviorTree(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CompileBehaviorTree", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::ListDataAssets(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListDataAssets"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
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
CommandletBlueprintReader::ReadDataAsset(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadDataAsset", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::CreateDataAsset(std::string_view assetPath,
	std::string_view className) {
	auto j = RunOp({L"-Op=CreateDataAsset",
					L"-Asset=" + Widen(assetPath),
					L"-Class=" + Widen(className)});
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
CommandletBlueprintReader::SetDataAssetProperty(std::string_view assetPath,
	std::string_view propertyName, std::string_view value) {
	auto j = RunOp({L"-Op=SetDataAssetProperty",
					L"-Asset="    + Widen(assetPath),
					L"-Property=" + Widen(propertyName),
					L"-Value="    + Widen(value)});
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
CommandletBlueprintReader::ListStateTrees(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListStateTrees"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
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
CommandletBlueprintReader::ReadStateTree(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadStateTree", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddStateTreeState(std::string_view assetPath,
	std::string_view parentStateId, std::string_view name) {
	std::vector<std::wstring> args = {L"-Op=AddStateTreeState",
									   L"-Asset=" + Widen(assetPath),
									   L"-Name="  + Widen(name)};
	if (!parentStateId.empty())
	{
		args.push_back(L"-Parent=" + Widen(parentStateId));
	}
	auto j = RunOp(args);
	AddStateTreeStateResult out;
	out.assetPath = std::string(assetPath);
	out.name      = std::string(name);
	if (j.is_object()) out.stateId = j.value("state_id", std::string{});
	return out;
}

IBlueprintReader::SetStateTreeTransitionResult
CommandletBlueprintReader::SetStateTreeTransition(std::string_view assetPath,
	std::string_view fromStateId, std::string_view toStateId,
	std::string_view trigger) {
	auto j = RunOp({L"-Op=SetStateTreeTransition",
					L"-Asset="   + Widen(assetPath),
					L"-From="    + Widen(fromStateId),
					L"-To="      + Widen(toStateId),
					L"-Trigger=" + Widen(trigger)});
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
CommandletBlueprintReader::CompileStateTree(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CompileStateTree", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::StartProfile(std::string_view mode) {
	auto j = RunOp({L"-Op=StartProfile", L"-Mode=" + Widen(mode)});
	StartProfileResult out;
	if (j.is_object()) {
		out.started    = j.value("started", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::StopProfileResult
CommandletBlueprintReader::StopProfile() {
	auto j = RunOp({L"-Op=StopProfile"});
	StopProfileResult out;
	if (j.is_object()) {
		out.stopped    = j.value("stopped", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::StatGroupResult
CommandletBlueprintReader::GetStats(std::string_view group) {
	auto j = RunOp({L"-Op=GetStats", L"-Group=" + Widen(group)});
	StatGroupResult out;
	out.group = std::string(group);
	if (j.is_object()) out.snapshot = j.value("snapshot", std::string{});
	return out;
}

IBlueprintReader::ScreenshotResult
CommandletBlueprintReader::TakeScreenshot(std::string_view destPath, int width, int height) {
	auto j = RunOp({L"-Op=TakeScreenshot",
					L"-Dest="   + Widen(destPath),
					L"-Width="  + std::to_wstring(width),
					L"-Height=" + std::to_wstring(height)});
	ScreenshotResult out;
	if (j.is_object()) {
		out.captured   = j.value("captured", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::CookResult
CommandletBlueprintReader::CookContent(std::string_view platform) {
	auto j = RunOp({L"-Op=CookContent", L"-Platform=" + Widen(platform)});
	CookResult out;
	out.platform = std::string(platform);
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::CookResult
CommandletBlueprintReader::PackageProject(std::string_view platform, std::string_view outputDir) {
	auto j = RunOp({L"-Op=PackageProject",
					L"-Platform=" + Widen(platform),
					L"-Output="   + Widen(outputDir)});
	CookResult out;
	out.platform = std::string(platform);
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::ClassInfo
CommandletBlueprintReader::IntrospectClass(std::string_view className) {
	auto j = RunOp({L"-Op=IntrospectClass", L"-Class=" + Widen(className)});
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
CommandletBlueprintReader::FindClass(std::string_view query) {
	auto j = RunOp({L"-Op=FindClass", L"-Query=" + Widen(query)});
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
CommandletBlueprintReader::ListFunctions(std::string_view className) {
	auto j = RunOp({L"-Op=ListFunctions", L"-Class=" + Widen(className)});
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
CommandletBlueprintReader::FocusActor(std::string_view actorName) {
	auto j = RunOp({L"-Op=FocusActor", L"-Actor=" + Widen(actorName)});
	FocusActorResult out;
	out.actorName = std::string(actorName);
	if (j.is_object())
	{
		out.focused = j.value("focused", false);
	}
	return out;
}

IBlueprintReader::SetCameraResult
CommandletBlueprintReader::SetCameraTransform(double lx, double ly, double lz,
											  double rp, double ry, double rr) {
	auto j = RunOp({L"-Op=SetCameraTransform",
					L"-LX=" + std::to_wstring(lx),
					L"-LY=" + std::to_wstring(ly),
					L"-LZ=" + std::to_wstring(lz),
					L"-RP=" + std::to_wstring(rp),
					L"-RY=" + std::to_wstring(ry),
					L"-RR=" + std::to_wstring(rr)});
	SetCameraResult out;
	if (j.is_object())
	{
		out.moved = j.value("moved", false);
	}
	return out;
}

IBlueprintReader::ViewportScreenshotResult
CommandletBlueprintReader::TakeViewportScreenshot(std::string_view destPath) {
	auto j = RunOp({L"-Op=TakeViewportScreenshot", L"-Dest=" + Widen(destPath)});
	ViewportScreenshotResult out;
	if (j.is_object()) {
		out.captured   = j.value("captured", false);
		out.outputFile = j.value("output_file", std::string{});
	}
	return out;
}

IBlueprintReader::SetShowFlagResult
CommandletBlueprintReader::SetShowFlag(std::string_view flagName, bool enabled) {
	auto j = RunOp({L"-Op=SetShowFlag",
					L"-Flag=" + Widen(flagName),
					L"-Enabled=" + std::wstring(enabled ? L"1" : L"0")});
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
CommandletBlueprintReader::ListNiagaraSystems(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListNiagaraSystems"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::NiagaraSystemInfo
CommandletBlueprintReader::ReadNiagaraSystem(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadNiagaraSystem", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::CreateNiagaraSystem(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CreateNiagaraSystem", L"-Asset=" + Widen(assetPath)});
	CreateNiagaraSystemResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.created        = j.value("created", false);
		out.alreadyExisted = j.value("already_existed", false);
	}
	return out;
}

IBlueprintReader::SetNiagaraParameterResult
CommandletBlueprintReader::SetNiagaraParameter(std::string_view assetPath,
	std::string_view parameterName, std::string_view value) {
	auto j = RunOp({L"-Op=SetNiagaraParameter",
					L"-Asset=" + Widen(assetPath),
					L"-Param=" + Widen(parameterName),
					L"-Value=" + Widen(value)});
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
CommandletBlueprintReader::ListLevelSequences(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListLevelSequences"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::LevelSequenceInfo
CommandletBlueprintReader::ReadLevelSequence(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadLevelSequence", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddSequenceTrack(std::string_view assetPath,
	std::string_view trackClass, std::string_view trackName) {
	auto j = RunOp({L"-Op=AddSequenceTrack",
					L"-Asset=" + Widen(assetPath),
					L"-Class=" + Widen(trackClass),
					L"-Name="  + Widen(trackName)});
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
CommandletBlueprintReader::SetSequencePlaybackRange(std::string_view assetPath,
	double startSeconds, double endSeconds) {
	auto j = RunOp({L"-Op=SetSequencePlaybackRange",
					L"-Asset=" + Widen(assetPath),
					L"-Start=" + std::to_wstring(startSeconds),
					L"-End="   + std::to_wstring(endSeconds)});
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
CommandletBlueprintReader::ListGameplayTags(std::string_view filter) {
	std::vector<std::wstring> args = {L"-Op=ListGameplayTags"};
	if (!filter.empty())
	{
		args.push_back(L"-Filter=" + Widen(filter));
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
CommandletBlueprintReader::AddGameplayTag(std::string_view tagName,
	std::string_view comment) {
	std::vector<std::wstring> args = {L"-Op=AddGameplayTag",
									   L"-Tag=" + Widen(tagName)};
	if (!comment.empty())
	{
		args.push_back(L"-Comment=" + Widen(comment));
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
CommandletBlueprintReader::ReadAbilitySet(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadAbilitySet", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::ListAnimBlueprints(std::string_view path) {
	std::vector<std::wstring> args = {L"-Op=ListAnimBlueprints"};
	if (!path.empty())
	{
		args.push_back(L"-Path=" + Widen(path));
	}
	auto j = RunOp(args);
	std::vector<BPAssetSummary> out;
	if (j.is_array()) {
		for (const auto& v : j) { BPAssetSummary s; from_json(v, s); out.push_back(std::move(s)); }
	}
	return out;
}

IBlueprintReader::AnimBlueprintInfo
CommandletBlueprintReader::ReadAnimBlueprint(std::string_view assetPath) {
	auto j = RunOp({L"-Op=ReadAnimBlueprint", L"-Asset=" + Widen(assetPath)});
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
CommandletBlueprintReader::AddAnimState(std::string_view assetPath,
	std::string_view stateMachine, std::string_view stateName) {
	auto j = RunOp({L"-Op=AddAnimState",
					L"-Asset="   + Widen(assetPath),
					L"-Machine=" + Widen(stateMachine),
					L"-Name="    + Widen(stateName)});
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
CommandletBlueprintReader::CompileAnimBlueprint(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CompileAnimBlueprint", L"-Asset=" + Widen(assetPath)});
	CompileAnimBlueprintResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object())
	{
		out.compiled = j.value("compiled", false);
	}
	return out;
}

// ----- Live editor ops ----------------------------------------------------

IBlueprintReader::ConsoleCommandResult
CommandletBlueprintReader::ConsoleCommand(std::string_view command) {
	auto j = RunOp({L"-Op=ConsoleCommand",
					L"-Command=" + Widen(command)});
	ConsoleCommandResult out;
	if (j.is_object()) out.output = j.value("output", std::string{});
	return out;
}

IBlueprintReader::CVarValue
CommandletBlueprintReader::GetCVar(std::string_view name) {
	auto j = RunOp({L"-Op=GetCVar", L"-Name=" + Widen(name)});
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
CommandletBlueprintReader::SetCVar(std::string_view name, std::string_view value) {
	auto j = RunOp({L"-Op=SetCVar",
					L"-Name="  + Widen(name),
					L"-Value=" + Widen(value)});
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
CommandletBlueprintReader::PieStart(std::string_view mode) {
	std::vector<std::wstring> args = {L"-Op=PieStart"};
	if (!mode.empty())
	{
		args.push_back(L"-Mode=" + Widen(mode));
	}
	auto j = RunOp(args);
	PieResult out;
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.mode    = j.value("mode",    std::string{});
	}
	return out;
}

IBlueprintReader::PieResult CommandletBlueprintReader::PieStop() {
	auto j = RunOp({L"-Op=PieStop"});
	PieResult out;
	if (j.is_object())
	{
		out.stopped = j.value("stopped", false);
	}
	return out;
}

// ----- Phase 8 EA-pull Wave 1 (partial) -----------------------------

IBlueprintReader::OpenAssetsResult CommandletBlueprintReader::ListOpenAssets() {
	auto j = RunOp({L"-Op=ListOpenAssets"});
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

IBlueprintReader::ActiveAssetResult CommandletBlueprintReader::GetActiveAsset() {
	auto j = RunOp({L"-Op=GetActiveAsset"});
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
CommandletBlueprintReader::GetCompileStatus(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetCompileStatus",
					L"-Asset=" + Widen(assetPath)});
	CompileStatusResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.status           = j.value("status", std::string{});
		out.lastCompileError = j.value("last_compile_error", std::string{});
	}
	return out;
}

IBlueprintReader::DirtyPackagesResult
CommandletBlueprintReader::GetDirtyPackages() {
	auto j = RunOp({L"-Op=GetDirtyPackages"});
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
CommandletBlueprintReader::GetFocusedWindow() {
	auto j = RunOp({L"-Op=GetFocusedWindow"});
	FocusedWindowResult out;
	if (j.is_object()) {
		out.title     = j.value("title",      std::string{});
		out.className = j.value("class_name", std::string{});
	}
	return out;
}

IBlueprintReader::PieStateResult CommandletBlueprintReader::GetPieState() {
	auto j = RunOp({L"-Op=GetPieState"});
	PieStateResult out;
	if (j.is_object()) {
		out.isPlaying     = j.value("is_playing",     false);
		out.mode          = j.value("mode",           std::string{});
		out.instanceCount = j.value("instance_count", 0);
	}
	return out;
}

IBlueprintReader::ModalStateResult CommandletBlueprintReader::GetModalState() {
	auto j = RunOp({L"-Op=GetModalState"});
	ModalStateResult out;
	if (j.is_object()) {
		out.isOpen = j.value("is_open", false);
		out.title  = j.value("title",   std::string{});
	}
	return out;
}

IBlueprintReader::EditorModesResult
CommandletBlueprintReader::GetActiveEditorMode() {
	auto j = RunOp({L"-Op=GetActiveEditorMode"});
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
CommandletBlueprintReader::GetFocusedWidget() {
	auto j = RunOp({L"-Op=GetFocusedWidget"});
	FocusedWidgetResult out;
	if (j.is_object()) {
		out.widgetType        = j.value("widget_type",         std::string{});
		out.parentWindowTitle = j.value("parent_window_title", std::string{});
	}
	return out;
}

IBlueprintReader::LiveCodingResult
CommandletBlueprintReader::LiveCodingCompile() {
	auto j = RunOp({L"-Op=LiveCodingCompile"});
	LiveCodingResult out;
	if (j.is_object()) {
		out.queued  = j.value("queued",  false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

IBlueprintReader::OpenAssetEditorResult
CommandletBlueprintReader::OpenAssetEditor(std::string_view assetPath) {
	auto j = RunOp({L"-Op=OpenAssetEditor",
					L"-Asset=" + Widen(assetPath)});
	OpenAssetEditorResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.opened = j.value("opened", false);
	}
	return out;
}

IBlueprintReader::CloseAssetEditorResult
CommandletBlueprintReader::CloseAssetEditor(std::string_view assetPath) {
	auto j = RunOp({L"-Op=CloseAssetEditor",
					L"-Asset=" + Widen(assetPath)});
	CloseAssetEditorResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.closed = j.value("closed", false);
	}
	return out;
}

IBlueprintReader::CameraTransformResult
CommandletBlueprintReader::GetCameraTransform() {
	auto j = RunOp({L"-Op=GetCameraTransform"});
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
CommandletBlueprintReader::GetViewMode() {
	auto j = RunOp({L"-Op=GetViewMode"});
	ViewModeResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.mode  = j.value("mode",  std::string{});
	}
	return out;
}

IBlueprintReader::ShowFlagsResult
CommandletBlueprintReader::GetShowFlags() {
	auto j = RunOp({L"-Op=GetShowFlags"});
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

IBlueprintReader::SelectedComponentsResult
CommandletBlueprintReader::GetSelectedComponents() {
	auto j = RunOp({L"-Op=GetSelectedComponents"});
	SelectedComponentsResult out;
	if (j.is_object()) {
		if (auto it = j.find("actors"); it != j.end() && it->is_array()) {
			for (const auto& a : *it) {
				if (!a.is_object()) continue;
				SelectedActorComponents ac;
				ac.actorName = a.value("actor_name", std::string{});
				if (auto cIt = a.find("components"); cIt != a.end() && cIt->is_array()) {
					for (const auto& c : *cIt) {
						if (!c.is_object()) continue;
						SelectedComponentInfo info;
						info.name           = c.value("name",            std::string{});
						info.componentClass = c.value("component_class", std::string{});
						ac.components.push_back(std::move(info));
					}
				}
				out.actors.push_back(std::move(ac));
			}
		}
	}
	return out;
}

namespace {
	IBlueprintReader::ContentBrowserSelectionResult
	ParseCBSelection(const nlohmann::json& j) {
		IBlueprintReader::ContentBrowserSelectionResult out;
		if (j.is_object()) {
			if (auto it = j.find("asset_paths"); it != j.end() && it->is_array()) {
				for (const auto& v : *it) {
					if (v.is_string()) out.assetPaths.push_back(v.get<std::string>());
				}
			}
		}
		return out;
	}
}    // namespace

IBlueprintReader::ContentBrowserSelectionResult
CommandletBlueprintReader::GetSelectedAssets() {
	return ParseCBSelection(RunOp({L"-Op=GetSelectedAssets"}));
}

IBlueprintReader::ContentBrowserSelectionResult
CommandletBlueprintReader::SetSelectedAssets(
		const std::vector<std::string>& assetPaths) {
	// Join with `;` because FParse::Value handles a single arg cleanly;
	// the plugin side splits on `;`. Path strings can't contain `;` in
	// UE's asset model so this is unambiguous.
	std::wstring joined;
	for (size_t i = 0; i < assetPaths.size(); ++i) {
		if (i > 0) joined += L";";
		joined += Widen(assetPaths[i]);
	}
	return ParseCBSelection(RunOp({L"-Op=SetSelectedAssets",
									L"-Assets=" + joined}));
}

IBlueprintReader::ContentBrowserFoldersResult
CommandletBlueprintReader::GetSelectedFolders() {
	auto j = RunOp({L"-Op=GetSelectedFolders"});
	ContentBrowserFoldersResult out;
	if (j.is_object()) {
		if (auto it = j.find("folder_paths"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string()) out.folderPaths.push_back(v.get<std::string>());
			}
		}
	}
	return out;
}

namespace {
	IBlueprintReader::ContentBrowserPathResult
	ParseCBPath(const nlohmann::json& j) {
		IBlueprintReader::ContentBrowserPathResult out;
		if (j.is_object()) {
			out.currentPath = j.value("current_path", std::string{});
		}
		return out;
	}
}    // namespace

IBlueprintReader::ContentBrowserPathResult
CommandletBlueprintReader::GetContentBrowserPath() {
	return ParseCBPath(RunOp({L"-Op=GetContentBrowserPath"}));
}

IBlueprintReader::ContentBrowserPathResult
CommandletBlueprintReader::SetContentBrowserPath(std::string_view folderPath) {
	return ParseCBPath(RunOp({L"-Op=SetContentBrowserPath",
								L"-Folder=" + Widen(folderPath)}));
}

IBlueprintReader::WorldToScreenResult
CommandletBlueprintReader::WorldToScreen(double x, double y, double z) {
	auto j = RunOp({L"-Op=WorldToScreen",
					L"-WX=" + std::to_wstring(x),
					L"-WY=" + std::to_wstring(y),
					L"-WZ=" + std::to_wstring(z)});
	WorldToScreenResult out;
	if (j.is_object()) {
		out.valid      = j.value("valid",        false);
		out.screenX    = j.value("screen_x",     0.0);
		out.screenY    = j.value("screen_y",     0.0);
		out.isOnScreen = j.value("is_on_screen", false);
	}
	return out;
}

IBlueprintReader::ScreenToWorldResult
CommandletBlueprintReader::ScreenToWorld(double x, double y, double d) {
	auto j = RunOp({L"-Op=ScreenToWorld",
					L"-SX=" + std::to_wstring(x),
					L"-SY=" + std::to_wstring(y),
					L"-Dist=" + std::to_wstring(d)});
	ScreenToWorldResult out;
	if (j.is_object()) {
		out.valid  = j.value("valid",  false);
		out.hit    = j.value("hit",    false);
		out.worldX = j.value("world_x", 0.0);
		out.worldY = j.value("world_y", 0.0);
		out.worldZ = j.value("world_z", 0.0);
		out.hitActorName = j.value("hit_actor_name", std::string{});
	}
	return out;
}

namespace {
	IBlueprintReader::UiSnapshotResult ParseUiSnapshot(const nlohmann::json& j) {
		IBlueprintReader::UiSnapshotResult out;
		if (j.is_object()) {
			out.truncated = j.value("truncated", false);
			if (auto it = j.find("nodes"); it != j.end() && it->is_array()) {
				for (const auto& n : *it) {
					if (!n.is_object()) continue;
					IBlueprintReader::UiNode node;
					node.depth        = n.value("depth",        0);
					node.widgetType   = n.value("widget_type",  std::string{});
					node.text         = n.value("text",         std::string{});
					node.parentWindow = n.value("parent_window", std::string{});
					out.nodes.push_back(std::move(node));
				}
			}
		}
		return out;
	}
}    // namespace

IBlueprintReader::UiSnapshotResult
CommandletBlueprintReader::UiSnapshot(std::string_view w, int d) {
	std::vector<std::wstring> args = {L"-Op=UiSnapshot",
									  L"-MaxDepth=" + std::to_wstring(d)};
	if (!w.empty()) {
		args.push_back(L"-Window=" + Widen(w));
	}
	return ParseUiSnapshot(RunOp(args));
}

IBlueprintReader::UiSnapshotResult
CommandletBlueprintReader::UiFind(std::string_view t, std::string_view r) {
	std::vector<std::wstring> args = {L"-Op=UiFind"};
	if (!t.empty()) args.push_back(L"-Text=" + Widen(t));
	if (!r.empty()) args.push_back(L"-Role=" + Widen(r));
	return ParseUiSnapshot(RunOp(args));
}

IBlueprintReader::DesktopWindowsResult
CommandletBlueprintReader::ListDesktopWindows() {
	auto j = RunOp({L"-Op=ListDesktopWindows"});
	DesktopWindowsResult out;
	if (j.is_object()) {
		if (auto it = j.find("windows"); it != j.end() && it->is_array()) {
			for (const auto& w : *it) {
				if (!w.is_object()) continue;
				DesktopWindowInfo info;
				info.title       = w.value("title",       std::string{});
				info.widgetType  = w.value("widget_type", std::string{});
				info.posX        = w.value("pos_x",       0.0);
				info.posY        = w.value("pos_y",       0.0);
				info.sizeX       = w.value("size_x",      0.0);
				info.sizeY       = w.value("size_y",      0.0);
				info.isActive    = w.value("is_active",   false);
				out.windows.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::GameFeaturesListResult
CommandletBlueprintReader::ListGameFeatures() {
	auto j = RunOp({L"-Op=ListGameFeatures"});
	GameFeaturesListResult out;
	if (j.is_object()) {
		if (auto it = j.find("features"); it != j.end() && it->is_array()) {
			for (const auto& f : *it) {
				if (!f.is_object()) continue;
				GameFeatureInfo info;
				info.pluginName = f.value("plugin_name", std::string{});
				info.pluginUrl  = f.value("plugin_url",  std::string{});
				info.state      = f.value("state",       std::string{});
				out.features.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::GameFeatureStateResult
CommandletBlueprintReader::GetGameFeatureState(std::string_view pluginName) {
	auto j = RunOp({L"-Op=GetGameFeatureState",
					L"-Plugin=" + Widen(pluginName)});
	GameFeatureStateResult out;
	out.pluginName = std::string(pluginName);
	if (j.is_object()) {
		out.valid     = j.value("valid",      false);
		out.state     = j.value("state",      std::string{});
		out.pluginUrl = j.value("plugin_url", std::string{});
	}
	return out;
}

IBlueprintReader::PluginListResult CommandletBlueprintReader::ListPlugins() {
	auto j = RunOp({L"-Op=ListPlugins"});
	PluginListResult out;
	if (j.is_object()) {
		if (auto it = j.find("plugins"); it != j.end() && it->is_array()) {
			for (const auto& p : *it) {
				if (!p.is_object()) continue;
				PluginInfo info;
				info.name           = p.value("name",            std::string{});
				info.descriptorPath = p.value("descriptor_path", std::string{});
				info.category       = p.value("category",        std::string{});
				info.version        = p.value("version",         std::string{});
				info.isEnabled      = p.value("is_enabled",      false);
				info.isBuiltIn      = p.value("is_built_in",     false);
				info.isContentOnly  = p.value("is_content_only", false);
				out.plugins.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::PluginDescriptorResult
CommandletBlueprintReader::GetPluginDescriptor(std::string_view pluginName) {
	auto j = RunOp({L"-Op=GetPluginDescriptor",
					L"-Plugin=" + Widen(pluginName)});
	PluginDescriptorResult out;
	out.name = std::string(pluginName);
	if (j.is_object()) {
		out.valid      = j.value("valid", false);
		if (j.contains("descriptor")) out.descriptor = j["descriptor"];
	}
	return out;
}

IBlueprintReader::PluginDependenciesResult
CommandletBlueprintReader::GetPluginDependencies(std::string_view pluginName) {
	auto j = RunOp({L"-Op=GetPluginDependencies",
					L"-Plugin=" + Widen(pluginName)});
	PluginDependenciesResult out;
	out.name = std::string(pluginName);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("dependencies"); it != j.end() && it->is_array()) {
			for (const auto& d : *it) {
				if (d.is_string()) out.dependencies.push_back(d.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::ActorAbilitiesResult
CommandletBlueprintReader::ListActorAbilities(std::string_view actorName) {
	auto j = RunOp({L"-Op=ListActorAbilities",
					L"-Actor=" + Widen(actorName)});
	ActorAbilitiesResult out;
	out.actorName = std::string(actorName);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("abilities"); it != j.end() && it->is_array()) {
			for (const auto& a : *it) {
				if (!a.is_object()) continue;
				ActorAbilityInfo info;
				info.abilityClass    = a.value("ability_class",   std::string{});
				info.isActive        = a.value("is_active",       false);
				info.level           = a.value("level",           1);
				info.instancedCount  = a.value("instanced_count", 0);
				out.abilities.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::ActorTagsResult
CommandletBlueprintReader::ListActorGameplayTags(std::string_view actorName) {
	auto j = RunOp({L"-Op=ListActorGameplayTags",
					L"-Actor=" + Widen(actorName)});
	ActorTagsResult out;
	out.actorName = std::string(actorName);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("tags"); it != j.end() && it->is_array()) {
			for (const auto& t : *it) {
				if (t.is_string()) out.tags.push_back(t.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::ActorAttributesResult
CommandletBlueprintReader::ListActorAttributes(std::string_view actorName) {
	auto j = RunOp({L"-Op=ListActorAttributes",
					L"-Actor=" + Widen(actorName)});
	ActorAttributesResult out;
	out.actorName = std::string(actorName);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("attributes"); it != j.end() && it->is_array()) {
			for (const auto& a : *it) {
				if (!a.is_object()) continue;
				ActorAttributeInfo info;
				info.name         = a.value("name",          std::string{});
				info.baseValue    = a.value("base_value",    0.0);
				info.currentValue = a.value("current_value", 0.0);
				out.attributes.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::ActorEffectsResult
CommandletBlueprintReader::ListActorGameplayEffects(std::string_view actorName) {
	auto j = RunOp({L"-Op=ListActorGameplayEffects",
					L"-Actor=" + Widen(actorName)});
	ActorEffectsResult out;
	out.actorName = std::string(actorName);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("effects"); it != j.end() && it->is_array()) {
			for (const auto& e : *it) {
				if (!e.is_object()) continue;
				ActorEffectInfo info;
				info.effectClass       = e.value("effect_class",        std::string{});
				info.stackCount        = e.value("stack_count",          1);
				info.durationRemaining = e.value("duration_remaining",   0.0);
				info.level             = e.value("level",                1.0);
				if (auto tIt = e.find("granted_tags"); tIt != e.end() && tIt->is_array()) {
					for (const auto& t : *tIt) {
						if (t.is_string()) info.grantedTags.push_back(t.get<std::string>());
					}
				}
				out.effects.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::BlueprintEditorStateResult
CommandletBlueprintReader::GetBlueprintEditorState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetBlueprintEditorState",
					L"-Asset=" + Widen(assetPath)});
	BlueprintEditorStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid             = j.value("valid",              false);
		out.currentGraphName  = j.value("current_graph_name", std::string{});
		out.compileStatus     = j.value("compile_status",     std::string{});
		if (auto it = j.find("selected_node_ids"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				if (n.is_string()) out.selectedNodeIds.push_back(n.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::MaterialInstanceParamsResult
CommandletBlueprintReader::GetMaterialInstanceParams(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetMaterialInstanceParams",
					L"-Asset=" + Widen(assetPath)});
	MaterialInstanceParamsResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid      = j.value("valid", false);
		out.parentPath = j.value("parent_path", std::string{});
		if (auto it = j.find("scalars"); it != j.end() && it->is_array()) {
			for (const auto& s : *it) {
				if (!s.is_object()) continue;
				MaterialInstanceScalarParam p;
				p.name  = s.value("name",  std::string{});
				p.value = s.value("value", 0.0);
				out.scalars.push_back(std::move(p));
			}
		}
		if (auto it = j.find("vectors"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (!v.is_object()) continue;
				MaterialInstanceVectorParam p;
				p.name = v.value("name", std::string{});
				p.r = v.value("r", 0.0); p.g = v.value("g", 0.0);
				p.b = v.value("b", 0.0); p.a = v.value("a", 0.0);
				out.vectors.push_back(std::move(p));
			}
		}
		if (auto it = j.find("textures"); it != j.end() && it->is_array()) {
			for (const auto& t : *it) {
				if (!t.is_object()) continue;
				MaterialInstanceTextureParam p;
				p.name        = t.value("name",         std::string{});
				p.texturePath = t.value("texture_path", std::string{});
				out.textures.push_back(std::move(p));
			}
		}
	}
	return out;
}

IBlueprintReader::StaticMeshInfoResult
CommandletBlueprintReader::GetStaticMeshInfo(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetStaticMeshInfo",
					L"-Asset=" + Widen(assetPath)});
	StaticMeshInfoResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid           = j.value("valid",            false);
		out.lodCount        = j.value("lod_count",        0);
		out.isNaniteEnabled = j.value("is_nanite_enabled", false);
		if (auto it = j.find("lods"); it != j.end() && it->is_array()) {
			for (const auto& l : *it) {
				if (!l.is_object()) continue;
				StaticMeshLODInfo info;
				info.triangleCount = l.value("triangle_count", 0);
				info.vertexCount   = l.value("vertex_count",   0);
				info.screenSize    = l.value("screen_size",    0.0);
				out.lods.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::UmgEditorStateResult
CommandletBlueprintReader::GetUmgEditorState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetUmgEditorState",
					L"-Asset=" + Widen(assetPath)});
	UmgEditorStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid              = j.value("valid",                false);
		out.currentDesignerTab = j.value("current_designer_tab", std::string{});
		if (auto it = j.find("selected_widget_names"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				if (n.is_string()) out.selectedWidgetNames.push_back(n.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::MaterialEditorStateResult
CommandletBlueprintReader::GetMaterialEditorState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetMaterialEditorState",
					L"-Asset=" + Widen(assetPath)});
	MaterialEditorStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid              = j.value("valid",                false);
		out.selectedNodeCount  = j.value("selected_node_count",  0);
		if (auto it = j.find("selected_expression_classes"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				if (n.is_string()) out.selectedExpressionClasses.push_back(n.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::MeshPreviewStateResult
CommandletBlueprintReader::GetMeshPreviewState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetMeshPreviewState",
					L"-Asset=" + Widen(assetPath)});
	MeshPreviewStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid             = j.value("valid",              false);
		out.currentLODLevel   = j.value("current_lod_level",  -1);
		out.currentLODIndex   = j.value("current_lod_index",  0);
	}
	return out;
}

IBlueprintReader::CinematicCameraResult
CommandletBlueprintReader::GetCinematicCamera() {
	auto j = RunOp({L"-Op=GetCinematicCamera"});
	CinematicCameraResult out;
	if (j.is_object()) {
		out.valid     = j.value("valid",      false);
		out.actorName = j.value("actor_name", std::string{});
		out.locX  = j.value("loc_x", 0.0);
		out.locY  = j.value("loc_y", 0.0);
		out.locZ  = j.value("loc_z", 0.0);
		out.pitch = j.value("pitch", 0.0);
		out.yaw   = j.value("yaw",   0.0);
		out.roll  = j.value("roll",  0.0);
		out.fov   = j.value("fov",   0.0);
	}
	return out;
}

IBlueprintReader::SequencerStateResult
CommandletBlueprintReader::GetSequencerState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetSequencerState",
					L"-Asset=" + Widen(assetPath)});
	SequencerStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid                       = j.value("valid",                          false);
		out.playheadSeconds             = j.value("playhead_seconds",               0.0);
		out.playbackStatus              = j.value("playback_status",                std::string{});
		out.playbackRangeStartSeconds   = j.value("playback_range_start_seconds",   0.0);
		out.playbackRangeEndSeconds     = j.value("playback_range_end_seconds",     0.0);
	}
	return out;
}

IBlueprintReader::AnimEditorStateResult
CommandletBlueprintReader::GetAnimEditorState(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetAnimEditorState",
					L"-Asset=" + Widen(assetPath)});
	AnimEditorStateResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid              = j.value("valid",                 false);
		out.selectedBoneIndex  = j.value("selected_bone_index",   -1);
		out.selectedSocketName = j.value("selected_socket_name",  std::string{});
	}
	return out;
}

IBlueprintReader::NiagaraModuleSelectionResult
CommandletBlueprintReader::GetNiagaraModuleSelection(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetNiagaraModuleSelection",
					L"-Asset=" + Widen(assetPath)});
	NiagaraModuleSelectionResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		if (auto it = j.find("selected_module_names"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				if (n.is_string()) out.selectedModuleNames.push_back(n.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::CurveEditorSelectionResult
CommandletBlueprintReader::GetCurveEditorSelection(std::string_view assetPath) {
	auto j = RunOp({L"-Op=GetCurveEditorSelection",
					L"-Asset=" + Widen(assetPath)});
	CurveEditorSelectionResult out;
	out.assetPath = std::string(assetPath);
	if (j.is_object()) {
		out.valid             = j.value("valid",             false);
		out.selectedKeyCount  = j.value("selected_key_count", 0);
		if (auto it = j.find("selected_curve_names"); it != j.end() && it->is_array()) {
			for (const auto& n : *it) {
				if (n.is_string()) out.selectedCurveNames.push_back(n.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::BufferVizModeResult
CommandletBlueprintReader::GetBufferVisualizationMode() {
	auto j = RunOp({L"-Op=GetBufferVisualizationMode"});
	BufferVizModeResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.mode  = j.value("mode",  std::string{});
	}
	return out;
}

IBlueprintReader::GizmoStateResult
CommandletBlueprintReader::GetGizmoState() {
	auto j = RunOp({L"-Op=GetGizmoState"});
	GizmoStateResult out;
	if (j.is_object()) {
		out.valid      = j.value("valid",       false);
		out.mode       = j.value("mode",        std::string{});
		out.coordSpace = j.value("coord_space", std::string{});
	}
	return out;
}

IBlueprintReader::ViewportRealtimeResult
CommandletBlueprintReader::GetViewportRealtime() {
	auto j = RunOp({L"-Op=GetViewportRealtime"});
	ViewportRealtimeResult out;
	if (j.is_object()) {
		out.valid       = j.value("valid",        false);
		out.isRealtime  = j.value("is_realtime",  false);
	}
	return out;
}

IBlueprintReader::ViewportCameraSettingsResult
CommandletBlueprintReader::GetViewportCameraSettings() {
	auto j = RunOp({L"-Op=GetViewportCameraSettings"});
	ViewportCameraSettingsResult out;
	if (j.is_object()) {
		out.valid        = j.value("valid",         false);
		out.fov          = j.value("fov",           0.0);
		out.cameraSpeed  = j.value("camera_speed",  0.0);
		out.nearClip     = j.value("near_clip",     0.0);
		out.farClip      = j.value("far_clip",      0.0);
	}
	return out;
}

IBlueprintReader::SnappingSettingsResult
CommandletBlueprintReader::GetSnappingSettings() {
	auto j = RunOp({L"-Op=GetSnappingSettings"});
	SnappingSettingsResult out;
	if (j.is_object()) {
		out.valid              = j.value("valid",                 false);
		out.gridEnabled        = j.value("grid_enabled",          false);
		out.rotGridEnabled     = j.value("rot_grid_enabled",      false);
		out.snapVertices       = j.value("snap_vertices",         false);
		out.currentPosGridSize = j.value("current_pos_grid_size", 0);
		out.currentRotGridSize = j.value("current_rot_grid_size", 0);
		out.actorSnapDistance  = j.value("actor_snap_distance",   0.0);
		out.snapDistance       = j.value("snap_distance",         0.0);
	}
	return out;
}

IBlueprintReader::ActiveViewportResult
CommandletBlueprintReader::GetActiveViewport() {
	auto j = RunOp({L"-Op=GetActiveViewport"});
	ActiveViewportResult out;
	if (j.is_object()) {
		out.valid          = j.value("valid",           false);
		out.viewportIndex  = j.value("viewport_index",  -1);
		out.isPerspective  = j.value("is_perspective",  false);
		out.sizeX          = j.value("size_x",          0);
		out.sizeY          = j.value("size_y",          0);
	}
	return out;
}

IBlueprintReader::HiddenActorsResult
CommandletBlueprintReader::GetHiddenActors() {
	auto j = RunOp({L"-Op=GetHiddenActors"});
	HiddenActorsResult out;
	if (j.is_object()) {
		out.truncated = j.value("truncated", false);
		if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string()) out.actorNames.push_back(v.get<std::string>());
			}
		}
	}
	return out;
}

IBlueprintReader::VisibleActorsResult
CommandletBlueprintReader::GetVisibleActors(std::string_view classFilter,
											 double maxDistanceCm) {
	std::vector<std::wstring> args = {L"-Op=GetVisibleActors",
									   L"-Dist=" + std::to_wstring(maxDistanceCm)};
	if (!classFilter.empty()) {
		args.push_back(L"-ClassFilter=" + Widen(classFilter));
	}
	auto j = RunOp(args);
	VisibleActorsResult out;
	if (j.is_object()) {
		out.truncated = j.value("truncated", false);
		if (auto it = j.find("actors"); it != j.end() && it->is_array()) {
			for (const auto& a : *it) {
				if (!a.is_object()) continue;
				VisibleActorInfo info;
				info.name         = a.value("name",          std::string{});
				info.label        = a.value("label",         std::string{});
				info.actorClass   = a.value("actor_class",   std::string{});
				info.worldX       = a.value("world_x",       0.0);
				info.worldY       = a.value("world_y",       0.0);
				info.worldZ       = a.value("world_z",       0.0);
				info.distanceCm   = a.value("distance_cm",   0.0);
				info.screenX      = a.value("screen_x",      0.0);
				info.screenY      = a.value("screen_y",      0.0);
				info.hasScreenPos = a.value("has_screen_pos",false);
				out.actors.push_back(std::move(info));
			}
		}
	}
	return out;
}

IBlueprintReader::SetViewModeResult
CommandletBlueprintReader::SetViewMode(std::string_view mode) {
	auto j = RunOp({L"-Op=SetViewMode", L"-Mode=" + Widen(mode)});
	SetViewModeResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.mode  = j.value("mode", std::string{});
	}
	return out;
}
IBlueprintReader::SetGizmoModeResult
CommandletBlueprintReader::SetGizmoMode(std::string_view mode) {
	auto j = RunOp({L"-Op=SetGizmoMode", L"-Mode=" + Widen(mode)});
	SetGizmoModeResult out;
	if (j.is_object()) {
		out.valid = j.value("valid", false);
		out.mode  = j.value("mode", std::string{});
	}
	return out;
}
IBlueprintReader::SetViewportRealtimeResult
CommandletBlueprintReader::SetViewportRealtime(bool enabled) {
	std::vector<std::wstring> args = {L"-Op=SetViewportRealtime"};
	if (enabled) args.push_back(L"-Enabled");
	auto j = RunOp(args);
	SetViewportRealtimeResult out;
	if (j.is_object()) {
		out.valid      = j.value("valid", false);
		out.isRealtime = j.value("is_realtime", false);
	}
	return out;
}
IBlueprintReader::SetActorVisibilityResult
CommandletBlueprintReader::SetActorVisibility(std::string_view actorName, bool visible) {
	std::vector<std::wstring> args = {L"-Op=SetActorVisibility",
									   L"-Name=" + Widen(actorName)};
	if (visible) args.push_back(L"-Visible");
	auto j = RunOp(args);
	SetActorVisibilityResult out;
	if (j.is_object()) {
		out.valid   = j.value("valid", false);
		out.name    = j.value("name", std::string{});
		out.visible = j.value("visible", false);
	}
	return out;
}
IBlueprintReader::HiddenLayersResult
CommandletBlueprintReader::GetHiddenLayers() {
	auto j = RunOp({L"-Op=GetHiddenLayers"});
	HiddenLayersResult out;
	if (j.is_object()) {
		out.truncated = j.value("truncated", false);
		if (auto it = j.find("layer_names"); it != j.end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string()) out.layerNames.push_back(v.get<std::string>());
			}
		}
	}
	return out;
}
IBlueprintReader::SetLayerVisibilityResult
CommandletBlueprintReader::SetLayerVisibility(std::string_view layer, bool visible) {
	std::vector<std::wstring> args = {L"-Op=SetLayerVisibility",
									   L"-Layer=" + Widen(layer)};
	if (visible) args.push_back(L"-Visible");
	auto j = RunOp(args);
	SetLayerVisibilityResult out;
	if (j.is_object()) {
		out.valid   = j.value("valid", false);
		out.layer   = j.value("layer", std::string{});
		out.visible = j.value("visible", false);
	}
	return out;
}

IBlueprintReader::SelectionResult
CommandletBlueprintReader::GetSelectedActors() {
	auto j = RunOp({L"-Op=GetSelectedActors"});
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

BPRJson CommandletBlueprintReader::GetEditorState() {
	// The plugin op returns the full shape already; pass through verbatim
	// so the tools/call layer just forwards it. Strip the leading `ok`
	// bool — the MCP envelope handles error semantics separately.
	auto j = RunOp({L"-Op=GetEditorState"});
	if (j.is_object()) {
		j.erase("ok");
	}
	return j;
}

IBlueprintReader::PythonResult
CommandletBlueprintReader::RunPythonScript(std::string_view code) {
	// Code goes over the wire as a -Code= arg. The arg encoder inner-
	// quotes whitespace values so multi-line scripts survive intact.
	std::wstring codeW(code.begin(), code.end());
	auto j = RunOp({L"-Op=RunPythonScript",
					L"-Code=" + codeW});
	PythonResult out;
	if (j.is_object()) {
		if (auto it = j.find("ok"); it != j.end() && it->is_boolean()) {
			out.ok = it->get<bool>();
		}
		if (auto it = j.find("error"); it != j.end() && it->is_string()) {
			out.error = it->get<std::string>();
		}
		if (auto it = j.find("command_result"); it != j.end() && it->is_string()) {
			out.commandResult = it->get<std::string>();
		}
		if (auto it = j.find("log"); it != j.end()) {
			out.log = *it;
		}
	}
	return out;
}

// Helper: pull a flat array of strings from a JSON field into a
// std::vector. Used by the asset-graph queries below.
static std::vector<std::string> ExtractStringArray(
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
CommandletBlueprintReader::GetReferencers(std::string_view assetPath) {
	std::wstring p(assetPath.begin(), assetPath.end());
	auto j = RunOp({L"-Op=GetReferencers", L"-Asset=" + p});
	AssetGraphResult out;
	out.packagePaths = ExtractStringArray(j, "referencers");
	return out;
}

IBlueprintReader::AssetGraphResult
CommandletBlueprintReader::GetDependencies(std::string_view assetPath) {
	std::wstring p(assetPath.begin(), assetPath.end());
	auto j = RunOp({L"-Op=GetDependencies", L"-Asset=" + p});
	AssetGraphResult out;
	out.packagePaths = ExtractStringArray(j, "dependencies");
	return out;
}

IBlueprintReader::ConfigReadResult
CommandletBlueprintReader::ReadConfigValue(std::string_view section,
										   std::string_view key,
										   std::string_view file) {
	std::wstring s(section.begin(), section.end());
	std::wstring k(key.begin(), key.end());
	std::wstring f(file.begin(), file.end());
	auto j = RunOp({L"-Op=ReadConfigValue",
					L"-Section=" + s,
					L"-Key=" + k,
					L"-File=" + f});
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
CommandletBlueprintReader::SetConfigValue(std::string_view section,
										  std::string_view key,
										  std::string_view value,
										  std::string_view file) {
	std::wstring s(section.begin(), section.end());
	std::wstring k(key.begin(), key.end());
	std::wstring v(value.begin(), value.end());
	std::wstring f(file.begin(), file.end());
	auto j = RunOp({L"-Op=SetConfigValue",
					L"-Section=" + s,
					L"-Key=" + k,
					L"-Value=" + v,
					L"-File=" + f});
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
CommandletBlueprintReader::BuildLighting(std::string_view quality) {
	std::wstring q(quality.begin(), quality.end());
	auto j = RunOp({L"-Op=BuildLighting", L"-Quality=" + q});
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
CommandletBlueprintReader::SetSelection(const std::vector<std::string>& actorNames,
										bool replace) {
	// Names are passed as a comma-joined list. None of them should
	// contain commas (UE actor names can't), so this is safe.
	std::wstring joined;
	for (std::size_t i = 0; i < actorNames.size(); ++i) {
		if (i)
		{
			joined += L",";
		}
		joined += Widen(actorNames[i]);
	}
	std::vector<std::wstring> args = {L"-Op=SetSelection",
									   L"-Names=" + joined};
	if (!replace)
	{
		args.push_back(L"-Add");
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
CommandletBlueprintReader::SpawnActor(std::string_view classPath,
	double locX, double locY, double locZ,
	double rotPitch, double rotYaw, double rotRoll,
	double scaleX, double scaleY, double scaleZ) {
	auto j = RunOp({
		L"-Op=SpawnActor",
		L"-Class=" + Widen(classPath),
		L"-LocX=" + std::to_wstring(locX),
		L"-LocY=" + std::to_wstring(locY),
		L"-LocZ=" + std::to_wstring(locZ),
		L"-RotPitch=" + std::to_wstring(rotPitch),
		L"-RotYaw="   + std::to_wstring(rotYaw),
		L"-RotRoll="  + std::to_wstring(rotRoll),
		L"-ScaleX=" + std::to_wstring(scaleX),
		L"-ScaleY=" + std::to_wstring(scaleY),
		L"-ScaleZ=" + std::to_wstring(scaleZ),
	});
	SpawnActorResult out;
	if (j.is_object()) {
		out.actorName  = j.value("actor_name",  std::string{});
		out.actorLabel = j.value("actor_label", std::string{});
	}
	return out;
}

void CommandletBlueprintReader::SetActorTransform(std::string_view actorName,
	double locX, double locY, double locZ,
	double rotPitch, double rotYaw, double rotRoll,
	double scaleX, double scaleY, double scaleZ) {
	(void)RunOp({
		L"-Op=SetActorTransform",
		L"-Name=" + Widen(actorName),
		L"-LocX=" + std::to_wstring(locX),
		L"-LocY=" + std::to_wstring(locY),
		L"-LocZ=" + std::to_wstring(locZ),
		L"-RotPitch=" + std::to_wstring(rotPitch),
		L"-RotYaw="   + std::to_wstring(rotYaw),
		L"-RotRoll="  + std::to_wstring(rotRoll),
		L"-ScaleX=" + std::to_wstring(scaleX),
		L"-ScaleY=" + std::to_wstring(scaleY),
		L"-ScaleZ=" + std::to_wstring(scaleZ),
	});
}

IBlueprintReader::DeleteActorResult
CommandletBlueprintReader::DeleteActor(std::string_view actorName) {
	auto j = RunOp({L"-Op=DeleteActor", L"-Name=" + Widen(actorName)});
	DeleteActorResult out;
	if (j.is_object())
	{
		out.deleted = j.value("deleted", false);
	}
	return out;
}

IBlueprintReader::OutputLogResult
CommandletBlueprintReader::ReadOutputLog(int limit, std::string_view minSeverity) {
	std::vector<std::wstring> args = {L"-Op=ReadOutputLog",
									   L"-Limit=" + std::to_wstring(limit)};
	if (!minSeverity.empty()) {
		args.push_back(L"-MinSeverity=" + Widen(minSeverity));
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
CommandletBlueprintReader::RunAutomationTests(std::string_view pattern) {
	std::vector<std::wstring> args = {L"-Op=RunAutomationTests"};
	if (!pattern.empty())
	{
		args.push_back(L"-Pattern=" + Widen(pattern));
	}
	auto j = RunOp(args);
	AutomationRunResult out;
	if (j.is_object()) {
		out.started = j.value("started", false);
		out.message = j.value("message", std::string{});
	}
	return out;
}

// ----- Batch sentinels (A1) -------------------------------------------------
void CommandletBlueprintReader::BeginBatch() {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=BeginBatch");
	(void)RunOp(args);
}

nlohmann::json CommandletBlueprintReader::EndBatch(bool skipCompile) {
	std::vector<std::wstring> args;
	args.push_back(L"-Op=EndBatch");
	if (skipCompile) {
		args.push_back(L"-Skip");
	}
	return RunOp(args);
}

nlohmann::json CommandletBlueprintReader::ShutdownDaemon() {
	// Hold the daemon mutex so a concurrent tool call can't half-spawn
	// a new daemon while we're tearing the current one down. After we
	// release, the next call's EnsureDaemonAttached() either attaches
	// to an externally-launched daemon or spawns a fresh process.
	std::lock_guard<std::mutex> lock(daemonMutex_);
	bool hadSocket = (socket_ != nullptr);
	// Drop the socket first so its destructor releases the TCP
	// connection before we terminate the child. The daemon's side
	// will notice the disconnect and clean up its per-connection state.
	socket_.reset();
#if defined(_WIN32)
	bool wasRunning = hadSocket || (daemonProcess_ != nullptr);
	if (daemonProcess_ != nullptr) {
		TerminateDaemon();
	}
	return nlohmann::json{
		{"ok", true},
		{"was_running", wasRunning},
		{"hint", "Next read tool call will auto-respawn (or re-attach to) the daemon."},
	};
#else    // defined(_WIN32)
	(void)hadSocket;
	return nlohmann::json{
		{"ok", true},
		{"was_running", false},
		{"hint", "Daemon mode is Windows-only; no-op on this platform."},
	};
#endif    // defined(_WIN32)
}

}    // namespace bpr::backends
