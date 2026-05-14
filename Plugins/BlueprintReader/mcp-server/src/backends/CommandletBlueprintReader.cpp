#include "backends/CommandletBlueprintReader.h"

#include "backends/CommandletArgEncoding.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
    #endif
    // winsock2.h must come before windows.h; otherwise it tries to pull
    // in winsock.h via windows.h and the symbols clash. Daemon-attach
    // path does an inline TCP probe before constructing the
    // SocketBlueprintReader.
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "Ws2_32.lib")
#else
    #include <signal.h>
#endif

namespace bpr::backends {

namespace {

#if defined(_WIN32)

std::wstring Widen(std::string_view s) {
    if (s.empty()) return L"";
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
    while (iss >> tok) out.push_back(Widen(tok));
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
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
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
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return;
            if (avail == 0) return;
            char buf[1024];
            DWORD got = 0;
            DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
            if (!ReadFile(h, buf, toRead, &got, nullptr) || got == 0) return;
            AppendTail(tail, buf, got);
        }
    };

    for (;;) {
        DWORD waitMs = 100;
        DWORD wr = WaitForSingleObject(pi.hProcess, waitMs);
        drain(outR, res.stdoutTail);
        drain(errR, res.stderrTail);
        if (wr == WAIT_OBJECT_0) break;
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

#else // !_WIN32

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

#endif

std::string TrimLines(const std::string& s, std::size_t maxLines) {
    if (s.empty()) return s;
    std::deque<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            lines.emplace_back(s.substr(start, i - start));
            start = i + 1;
            if (lines.size() > maxLines) lines.pop_front();
        }
    }
    if (start < s.size()) {
        lines.emplace_back(s.substr(start));
        if (lines.size() > maxLines) lines.pop_front();
    }
    std::string out;
    for (const auto& l : lines) {
        out.append(l).push_back('\n');
    }
    return out;
}

std::filesystem::path TempJsonPath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::filesystem::path tmp = (n == 0) ? std::filesystem::path(L"C:\\Windows\\Temp")
                                         : std::filesystem::path(std::wstring(buf, n));
#else
    std::filesystem::path tmp = std::filesystem::temp_directory_path();
#endif
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream name;
    name << "bp-reader-" << std::hex << dist(rng) << ".json";
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
#else
    if (const char* v = std::getenv(key); v != nullptr && *v != '\0') return std::string(v);
    return fallback;
#endif
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
#endif
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
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#else
    // POSIX stub — flock-based version is a follow-up.
    (void)blockFor;
    return true;  // pretend acquired
#endif
}

void SpawnLock::Release() {
    if (!held_) return;
#if defined(_WIN32)
    if (handle_) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#endif
    held_ = false;
}

} // namespace

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
        const auto binDir = cfg_.engineDir / "Engine" / "Binaries" / "Win64";
        const std::string& cfgName = cfg_.editorConfig;  // "Development" / "DebugGame" / etc.
        std::filesystem::path candidate = (cfgName.empty() || cfgName == "Development")
            ? binDir / "UnrealEditor-Cmd.exe"
            : binDir / fmt::format("UnrealEditor-Win64-{}-Cmd.exe", cfgName);
        if (!std::filesystem::exists(candidate)) {
            throw BlueprintReaderError(fmt::format(
                "UnrealEditor-Cmd ({} config) not found at: {}\n"
                "Hint: build the editor target in '{}' configuration, OR set "
                "BP_READER_EDITOR_CONFIG to a config you've already built "
                "(Development is the default).",
                cfgName.empty() ? "Development" : cfgName,
                candidate.string(),
                cfgName.empty() ? "Development" : cfgName));
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
#endif
}

namespace {

// Convert an op-args vector of wstrings (the existing internal format
// used by every typed method on this class) into UTF-8 strings the
// socket reader's `RunOpRaw` accepts. The daemon's TCP server runs
// FParse against the joined arg string, just like the in-process
// commandlet, so we don't need to strip Windows-style outer quoting
// here — the args arrive structured (one per element).
std::vector<std::string> ToUtf8Args(const std::vector<std::wstring>& w) {
    std::vector<std::string> out;
    out.reserve(w.size());
    for (const auto& s : w) out.push_back(Narrow(s));
    return out;
}

// Win32 PID-aliveness probe. Used by TryAttachExistingDaemon to drop
// stale handshake files that survive a daemon crash. Returns false on
// any failure so a missing/uncertain answer is treated as "dead."
bool ProcessAlive(int pid) {
#if defined(_WIN32)
    if (pid <= 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
#endif
}

} // namespace

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
#endif
            return RunOpOneShot(opArgs);
        }
    }
    return RunOpOneShot(opArgs);
}

nlohmann::json CommandletBlueprintReader::RunOpOneShot(const std::vector<std::wstring>& opArgs) {
    auto outFile = TempJsonPath();

    std::vector<std::wstring> args;
    args.reserve(opArgs.size() + 8);
    args.push_back(cfg_.uproject.wstring());
    args.push_back(L"-run=BlueprintReader");
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
    for (auto& extra : SplitArgs(cfg_.editorExtraArgs)) args.push_back(std::move(extra));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = RunChild(editorCmdExe_.wstring(), args, cfg_.timeout);
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::fprintf(stderr,
                 "[bp-reader-mcp][commandlet] op-args=%zu exit=%lu timed_out=%d duration=%lldms\n",
                 opArgs.size(), static_cast<unsigned long>(r.exitCode),
                 r.timedOut ? 1 : 0, static_cast<long long>(dt));

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
    if (!std::filesystem::exists(hsPath, ec)) return nullptr;

    std::ifstream f(hsPath);
    if (!f) return nullptr;
    nlohmann::json j;
    try { f >> j; }
    catch (...) { return nullptr; }

    int pid = j.value("pid", 0);
    // pid is diagnostic-only on the editor side (lifetime lock is the
    // source of truth) but extremely useful here as a cheap "is the
    // daemon still alive?" probe before we sink a TCP connect.
    if (pid > 0 && !ProcessAlive(pid)) return nullptr;

    SocketBlueprintReader::Config sc;
    sc.host  = j.value("host",  std::string("127.0.0.1"));
    sc.port  = j.value("port",  0);
    sc.token = j.value("token", std::string());
    if (sc.port <= 0 || sc.token.empty()) return nullptr;

    // Wire the handshake-file path through to the socket reader so it
    // can self-refresh on connect-refused / auth-fail (issue #9 pattern
    // — daemon restart with a new port or token).
    sc.handshakeFilePath = hsPath.string();
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
            if (wsaInited) WSACleanup();
        });
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return nullptr;
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
            if (rc <= 0) return nullptr;
            int err = 0;
            int errLen = sizeof(err);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&err), &errLen);
            if (err != 0) return nullptr;
        }
    }
#endif

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
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // Caller's TryAttachExistingDaemon will return nullptr and
    // EnsureDaemonAttached will turn that into a useful error.
}

SocketBlueprintReader&
CommandletBlueprintReader::EnsureDaemonAttached() {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    if (socket_) return *socket_;

    // First: see if a daemon is already listening (could be one we
    // spawned earlier this session, or a separately-launched editor
    // running the daemon). The lifetime lock on the editor side makes
    // sure we won't see two of them at once.
    socket_ = TryAttachExistingDaemon();
    if (socket_) return *socket_;

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
#endif
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
    if (daemonProcess_ == nullptr) return;
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
    if (!cfg_.useDaemon) return;
    if (prewarmThread_.joinable()) return;  // already prewarming
    prewarmThread_ = std::thread([this]() {
        try {
            // EnsureDaemonAttached holds daemonMutex_; a real call
            // hitting EnsureDaemonAttached concurrently will block on
            // the same mutex and find a hot socket once we're done.
            (void)EnsureDaemonAttached();
            std::fprintf(stderr,
                "[bp-reader-mcp][commandlet][daemon] prewarm complete\n");
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
    args.push_back(L"-run=BlueprintReader");
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

    std::fprintf(stderr,
        "[bp-reader-mcp][commandlet][daemon] spawned UnrealEditor-Cmd "
        "(pid=%lu); waiting for handshake at "
        "<Project>/Saved/bp-reader-cmdlet.json (timeout=%llds)\n",
        static_cast<unsigned long>(pi.dwProcessId),
        static_cast<long long>(cfg_.startupTimeout.count()));
}

#else // !_WIN32

void CommandletBlueprintReader::TerminateDaemon() {}

void CommandletBlueprintReader::Prewarm() {
    // No-op on non-Windows; daemon mode is unsupported there.
}

void CommandletBlueprintReader::SpawnDaemon() {
    throw BlueprintReaderError("daemon mode is Windows-only");
}

#endif

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
    if (type.IsArray) args.push_back(L"-TypeIsArray");
    if (type.IsSet)   args.push_back(L"-TypeIsSet");
    if (type.IsMap)   args.push_back(L"-TypeIsMap");
    if (!defaultValue.empty()) {
        args.push_back(L"-Default=" + Widen(defaultValue));
    }
    if (!category.empty()) {
        args.push_back(L"-Category=" + Widen(category));
    }
    if (replicated) args.push_back(L"-Replicated");
    if (editable)   args.push_back(L"-Editable");
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
        if (v.empty()) continue;
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

namespace {
// Append pin-type flags (-TypeCategory, -TypeSubCategory, etc.) for a BPPinType.
void AppendPinTypeFlags(std::vector<std::wstring>& args, const BPPinType& type) {
    args.push_back(L"-TypeCategory=" + Widen(type.Category));
    if (type.SubCategory.has_value() && !type.SubCategory->empty()) {
        args.push_back(L"-TypeSubCategory=" + Widen(*type.SubCategory));
    }
    if (type.SubCategoryObject.has_value() && !type.SubCategoryObject->empty()) {
        args.push_back(L"-TypeSubCategoryObject=" + Widen(*type.SubCategoryObject));
    }
    if (type.IsArray) args.push_back(L"-TypeIsArray");
    if (type.IsSet)   args.push_back(L"-TypeIsSet");
    if (type.IsMap)   args.push_back(L"-TypeIsMap");
}
} // namespace

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
    if (newType.SubCategory)       args.push_back(L"-TypeSubCategory=" + Widen(*newType.SubCategory));
    if (newType.SubCategoryObject) args.push_back(L"-TypeSubCategoryObject=" + Widen(*newType.SubCategoryObject));
    if (newType.IsArray) args.push_back(L"-TypeIsArray");
    if (newType.IsSet)   args.push_back(L"-TypeIsSet");
    if (newType.IsMap)   args.push_back(L"-TypeIsMap");
    (void)RunOp(args);
}

void CommandletBlueprintReader::SetVariableCategory(std::string_view assetPath,
                                                    std::string_view name,
                                                    std::string_view category) {
    std::vector<std::wstring> args;
    args.push_back(L"-Op=SetVariableCategory");
    args.push_back(L"-Asset=" + Widen(assetPath));
    args.push_back(L"-Name="  + Widen(name));
    if (!category.empty()) args.push_back(L"-Category=" + Widen(category));
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
    if (createDirs) args.push_back(L"-CreateDirs");
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
    if (!dirtyOnly) args.push_back(L"-IncludeClean");
    auto j = RunOp(args);
    SaveAllResult out;
    if (j.is_object()) {
        out.savedCount = j.value("saved_count", 0);
        if (auto it = j.find("failed_assets"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.failedAssets.push_back(v.get<std::string>());
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
    if (force) args.push_back(L"-Force");
    auto j = RunOp(args);
    DeleteAssetResult out;
    out.path = std::string(assetPath);
    if (j.is_object()) {
        out.deleted = j.value("deleted", false);
        if (auto it = j.find("referencing_assets"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.referencingAssets.push_back(v.get<std::string>());
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
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
                if (v.is_string()) out.columns.push_back(v.get<std::string>());
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
    if (overwrite) args.push_back(L"-Overwrite");
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
    if (!parentName.empty()) args.push_back(L"-Parent=" + Widen(parentName));
    if (!socket.empty())     args.push_back(L"-Socket=" + Widen(socket));
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
    if (j.is_object()) out.removed = j.value("removed", false);
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
    if (!newParentName.empty()) args.push_back(L"-NewParent=" + Widen(newParentName));
    if (!socket.empty())        args.push_back(L"-Socket="    + Widen(socket));
    auto j = RunOp(args);
    AttachComponentResult out;
    out.assetPath     = std::string(assetPath);
    out.name          = std::string(name);
    out.newParentName = std::string(newParentName);
    out.socket        = std::string(socket);
    if (j.is_object()) out.reparented = j.value("reparented", false);
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
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
            if (v.is_string()) out.parameterNames.push_back(v.get<std::string>());
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
    if (j.is_object()) out.connected = j.value("connected", false);
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
    if (j.is_object()) out.compiled = j.value("compiled", false);
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
    if (!parentName.empty()) args.push_back(L"-Parent=" + Widen(parentName));
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
    if (j.is_object()) out.bound = j.value("bound", false);
    return out;
}

IBlueprintReader::CompileWidgetBlueprintResult
CommandletBlueprintReader::CompileWidgetBlueprint(std::string_view assetPath) {
    auto j = RunOp({L"-Op=CompileWidgetBlueprint", L"-Asset=" + Widen(assetPath)});
    CompileWidgetBlueprintResult out;
    out.assetPath = std::string(assetPath);
    if (j.is_object()) out.compiled = j.value("compiled", false);
    return out;
}

// ----- Behavior Tree authoring (Stage 2) ---------------------------------

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListBehaviorTrees(std::string_view path) {
    std::vector<std::wstring> args = {L"-Op=ListBehaviorTrees"};
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
    if (!parentNodeId.empty()) args.push_back(L"-Parent=" + Widen(parentNodeId));
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
    if (j.is_object()) out.compiled = j.value("compiled", false);
    return out;
}

// ----- DataAsset CRUD (Stage 2) ------------------------------------------

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListDataAssets(std::string_view path) {
    std::vector<std::wstring> args = {L"-Op=ListDataAssets"};
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
        if (auto it = j.find("properties"); it != j.end()) out.properties = *it;
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
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
    if (!parentStateId.empty()) args.push_back(L"-Parent=" + Widen(parentStateId));
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
    if (j.is_object()) out.added = j.value("added", false);
    return out;
}

IBlueprintReader::CompileStateTreeResult
CommandletBlueprintReader::CompileStateTree(std::string_view assetPath) {
    auto j = RunOp({L"-Op=CompileStateTree", L"-Asset=" + Widen(assetPath)});
    CompileStateTreeResult out;
    out.assetPath = std::string(assetPath);
    if (j.is_object()) out.compiled = j.value("compiled", false);
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
    if (!j.is_object()) return out;
    out.className   = j.value("class",  std::string{});
    out.parentClass = j.value("parent", std::string{});
    if (auto it = j.find("ancestors"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) if (a.is_string()) out.ancestors.push_back(a.get<std::string>());
    }
    if (auto it = j.find("properties"); it != j.end() && it->is_array()) {
        for (const auto& p : *it) {
            ClassPropertyInfo cp;
            cp.name     = p.value("name",     std::string{});
            cp.typeName = p.value("type",     std::string{});
            cp.category = p.value("category", std::string{});
            out.properties.push_back(std::move(cp));
        }
    }
    if (auto it = j.find("functions"); it != j.end() && it->is_array()) {
        for (const auto& f : *it) {
            ClassFunctionInfo cf;
            cf.name     = f.value("name",  std::string{});
            cf.flagsCsv = f.value("flags", std::string{});
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
                if (c.is_string()) out.classNames.push_back(c.get<std::string>());
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
    if (j.is_object()) out.focused = j.value("focused", false);
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
    if (j.is_object()) out.moved = j.value("moved", false);
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
    if (j.is_object()) out.enabled = j.value("enabled", false);
    return out;
}

// ----- Stage 4: Niagara / Sequencer / GAS / AnimGraph -------------------

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListNiagaraSystems(std::string_view path) {
    std::vector<std::wstring> args = {L"-Op=ListNiagaraSystems"};
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
        for (const auto& v : *it) if (v.is_string()) out.parameterNames.push_back(v.get<std::string>());
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
    if (j.is_object()) out.applied = j.value("applied", false);
    return out;
}

std::vector<BPAssetSummary>
CommandletBlueprintReader::ListLevelSequences(std::string_view path) {
    std::vector<std::wstring> args = {L"-Op=ListLevelSequences"};
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
    if (j.is_object()) out.added = j.value("added", false);
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
    if (j.is_object()) out.applied = j.value("applied", false);
    return out;
}

IBlueprintReader::GameplayTagListResult
CommandletBlueprintReader::ListGameplayTags(std::string_view filter) {
    std::vector<std::wstring> args = {L"-Op=ListGameplayTags"};
    if (!filter.empty()) args.push_back(L"-Filter=" + Widen(filter));
    auto j = RunOp(args);
    GameplayTagListResult out;
    if (j.is_object()) {
        if (auto it = j.find("tags"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) if (v.is_string()) out.tags.push_back(v.get<std::string>());
        }
    }
    return out;
}

IBlueprintReader::AddGameplayTagResult
CommandletBlueprintReader::AddGameplayTag(std::string_view tagName,
    std::string_view comment) {
    std::vector<std::wstring> args = {L"-Op=AddGameplayTag",
                                       L"-Tag=" + Widen(tagName)};
    if (!comment.empty()) args.push_back(L"-Comment=" + Widen(comment));
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
    if (!j.is_object()) return out;
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
    if (!path.empty()) args.push_back(L"-Path=" + Widen(path));
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
    if (!j.is_object()) return out;
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
    if (j.is_object()) out.added = j.value("added", false);
    return out;
}

IBlueprintReader::CompileAnimBlueprintResult
CommandletBlueprintReader::CompileAnimBlueprint(std::string_view assetPath) {
    auto j = RunOp({L"-Op=CompileAnimBlueprint", L"-Asset=" + Widen(assetPath)});
    CompileAnimBlueprintResult out;
    out.assetPath = std::string(assetPath);
    if (j.is_object()) out.compiled = j.value("compiled", false);
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
    if (!mode.empty()) args.push_back(L"-Mode=" + Widen(mode));
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
    if (j.is_object()) out.stopped = j.value("stopped", false);
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

IBlueprintReader::SelectionResult
CommandletBlueprintReader::GetSelectedActors() {
    auto j = RunOp({L"-Op=GetSelectedActors"});
    SelectionResult out;
    if (j.is_object()) {
        if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.actorNames.push_back(v.get<std::string>());
            }
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
        if (i) joined += L",";
        joined += Widen(actorNames[i]);
    }
    std::vector<std::wstring> args = {L"-Op=SetSelection",
                                       L"-Names=" + joined};
    if (!replace) args.push_back(L"-Add");
    auto j = RunOp(args);
    SelectionResult out;
    if (j.is_object()) {
        if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.actorNames.push_back(v.get<std::string>());
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
    if (j.is_object()) out.deleted = j.value("deleted", false);
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
                if (!e.is_object()) continue;
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
    if (!pattern.empty()) args.push_back(L"-Pattern=" + Widen(pattern));
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
#else
    (void)hadSocket;
    return nlohmann::json{
        {"ok", true},
        {"was_running", false},
        {"hint", "Daemon mode is Windows-only; no-op on this platform."},
    };
#endif
}

} // namespace bpr::backends
