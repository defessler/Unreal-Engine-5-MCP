#include "backends/CommandletBlueprintReader.h"

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
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
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

std::wstring BuildCommandLine(const std::wstring& exe,
                              const std::vector<std::wstring>& args) {
    std::wstring cmd = QuoteArg(exe);
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd.append(QuoteArg(a));
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
// any code between RunOpDaemon's "create file" and "drop file" throws.
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
    r.failureReason = "CommandletBlueprintReader is Windows-only in Phase 1.";
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
    // Join the prewarm thread first if it's still running. EnsureDaemon
    // holds daemonMutex_, so by the time join() returns the daemon is either
    // ready or its setup failed and the mutex is released. Either way we're
    // safe to terminate.
    if (prewarmThread_.joinable()) {
        prewarmThread_.join();
    }
#if defined(_WIN32)
    TerminateDaemon();
#endif
}

nlohmann::json CommandletBlueprintReader::RunOp(const std::vector<std::wstring>& opArgs) {
    if (cfg_.useDaemon) {
        try {
            return RunOpDaemon(opArgs);
        } catch (const AssetNotFound&) {
            throw;  // user-level error; propagate as-is
        } catch (const std::exception& e) {
            // Daemon transport failure — log and fall through to one-shot.
            std::fprintf(stderr,
                "[bp-reader-mcp][commandlet][daemon] transport error, falling back to one-shot: %s\n",
                e.what());
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

#if defined(_WIN32)

void CommandletBlueprintReader::TerminateDaemon() {
    if (daemonProcess_ != nullptr) {
        // Best-effort clean shutdown: send QUIT, then close stdin to signal EOF.
        if (daemonStdin_ != nullptr) {
            const char* quit = "QUIT\n";
            DWORD written = 0;
            WriteFile(daemonStdin_, quit, 5, &written, nullptr);
            CloseHandle(daemonStdin_);
            daemonStdin_ = nullptr;
        }
        // Wait briefly; if it doesn't exit, terminate.
        if (WaitForSingleObject(daemonProcess_, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(daemonProcess_, 0);
            WaitForSingleObject(daemonProcess_, 1000);
        }
        CloseHandle(daemonProcess_);
        daemonProcess_ = nullptr;
    }
    if (daemonStdin_ != nullptr) {
        CloseHandle(daemonStdin_);
        daemonStdin_ = nullptr;
    }
    if (daemonStdout_ != nullptr) {
        CloseHandle(daemonStdout_);
        daemonStdout_ = nullptr;
    }
    accumulator_.clear();
}

void CommandletBlueprintReader::Prewarm() {
    if (!cfg_.useDaemon) return;
    if (prewarmThread_.joinable()) return;  // already prewarming
    prewarmThread_ = std::thread([this]() {
        try {
            std::lock_guard<std::mutex> lock(daemonMutex_);
            // If a real tool call beat us to it, EnsureDaemon is a no-op.
            EnsureDaemon();
            std::fprintf(stderr,
                "[bp-reader-mcp][commandlet][daemon] prewarm complete\n");
        } catch (const std::exception& e) {
            // Swallow: the next real tool call will retry under its own lock.
            // Logging only — never let the prewarm thread crash main.
            std::fprintf(stderr,
                "[bp-reader-mcp][commandlet][daemon] prewarm failed: %s "
                "(tool calls will retry)\n", e.what());
        }
    });
}

void CommandletBlueprintReader::EnsureDaemon() {
    if (daemonProcess_ != nullptr) {
        // Sanity: if the child died unexpectedly, recycle. Use a 0-timeout
        // wait rather than GetExitCodeProcess + STILL_ACTIVE comparison —
        // the latter is fooled if the child legitimately exits with code
        // 259 (== STILL_ACTIVE).
        if (WaitForSingleObject(daemonProcess_, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(daemonProcess_, &code);
            std::fprintf(stderr,
                "[bp-reader-mcp][commandlet][daemon] child exited with code %lu; respawning\n",
                static_cast<unsigned long>(code));
            TerminateDaemon();
        } else {
            return;
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childInR = nullptr, childInW = nullptr;
    HANDLE childOutR = nullptr, childOutW = nullptr;
    if (!CreatePipe(&childInR, &childInW, &sa, 0)) {
        throw BlueprintReaderError("CreatePipe(stdin) failed");
    }
    if (!CreatePipe(&childOutR, &childOutW, &sa, 0)) {
        CloseHandle(childInR); CloseHandle(childInW);
        throw BlueprintReaderError("CreatePipe(stdout) failed");
    }
    SetHandleInformation(childInW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(childOutR, HANDLE_FLAG_INHERIT, 0);

    std::vector<std::wstring> args;
    args.push_back(cfg_.uproject.wstring());
    args.push_back(L"-run=BlueprintReader");
    args.push_back(L"-Daemon");
    args.push_back(L"-nullrhi");
    args.push_back(L"-nosplash");
    args.push_back(L"-unattended");
    args.push_back(L"-nopause");
    args.push_back(L"-stdout");
    for (auto& extra : SplitArgs(cfg_.editorExtraArgs)) args.push_back(std::move(extra));
    std::wstring cmd = BuildCommandLine(editorCmdExe_.wstring(), args);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = childInR;
    si.hStdOutput = childOutW;
    si.hStdError  = childOutW;  // merge stderr into stdout

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        editorCmdExe_.wstring().c_str(),
        cmd.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);
    CloseHandle(childInR);
    CloseHandle(childOutW);
    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(childInW);
        CloseHandle(childOutR);
        throw BlueprintReaderError(fmt::format(
            "CreateProcessW(daemon) failed (err={})", err));
    }
    CloseHandle(pi.hThread);

    daemonProcess_ = pi.hProcess;
    daemonStdin_   = childInW;
    daemonStdout_  = childOutR;

    // Wait for the daemon's READY sentinel before declaring it usable. Big UE
    // projects (lots of plugins, large content set, cold DDC) take much longer
    // than per-call ops do, so this gets its own bigger timeout.
    const auto t0 = std::chrono::steady_clock::now();
    try {
        ReadUntilMarker("__BPR_READY__\n", cfg_.startupTimeout);
    } catch (const std::exception& e) {
        // Persist the full daemon stderr/stdout to a known file before tearing
        // down — the inline tail (now 250 lines but still bounded) often
        // truncates the actual fatal when many other plugins log warnings
        // first. Users can read this file to see the whole story.
        std::filesystem::path logPath;
        try {
            logPath = std::filesystem::temp_directory_path() / "bp-reader-mcp-daemon-failure.log";
            std::ofstream logFile(logPath, std::ios::trunc);
            if (logFile) {
                logFile << "=== Daemon failed to reach READY ===\n";
                logFile << "Engine    : " << cfg_.engineDir.string() << "\n";
                logFile << "Project   : " << cfg_.uproject.string() << "\n";
                logFile << "ExtraArgs : " << cfg_.editorExtraArgs << "\n";
                logFile << "Error     : " << e.what() << "\n\n";
                logFile << "=== Full editor stdout/stderr (newest line last) ===\n";
                logFile << accumulator_;
            }
        } catch (...) {
            // Don't let logging-the-failure failure shadow the original failure.
            logPath.clear();
        }
        TerminateDaemon();

        const std::string what = e.what();
        const std::string logHint = logPath.empty()
            ? std::string{}
            : fmt::format("\nFull daemon log: {}", logPath.string());

        // The two failure modes have very different fixes — separate them so
        // the next reader of the message doesn't go chase the wrong one.
        if (what.find("process exited") != std::string::npos) {
            throw BlueprintReaderError(fmt::format(
                "daemon exited before reaching READY: {}{}\n"
                "Hint: scan the tail above (and the full log) for 'Error:' / "
                "'Fatal:' lines, OR for absence of any 'BlueprintReader' / "
                "'BlueprintReaderEditor' / 'Commandlet' messages — if you see "
                "no BlueprintReader logs, the UE plugin module probably isn't "
                "built (rebuild the editor target). For plugin/module load "
                "failures (e.g. 'Plugin X failed to load because module Y...'), "
                "set BP_READER_EDITOR_ARGS=\"-EnableAllPlugins\".",
                what, logHint));
        }
        throw BlueprintReaderError(fmt::format(
            "daemon timed out reaching READY (waited {}s; bump "
            "BP_READER_STARTUP_TIMEOUT_SECONDS for slower projects): {}{}",
            cfg_.startupTimeout.count(), what, logHint));
    }
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr,
        "[bp-reader-mcp][commandlet][daemon] READY after %lldms\n",
        static_cast<long long>(dt));
}

std::string CommandletBlueprintReader::ReadUntilMarker(
    const std::string& marker, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    for (;;) {
        auto pos = accumulator_.find(marker);
        if (pos != std::string::npos) {
            std::string consumed = accumulator_.substr(0, pos);
            accumulator_.erase(0, pos + marker.size());
            return consumed;
        }

        // Check if child died. Same code-259 caveat — WaitForSingleObject(0)
        // returns WAIT_OBJECT_0 only if the process is actually signaled
        // (i.e. exited).
        if (WaitForSingleObject(daemonProcess_, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(daemonProcess_, &code);
            throw BlueprintReaderError(fmt::format(
                "daemon process exited (code={}); tail:\n{}", code, TrimLines(accumulator_, 250)));
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            throw BlueprintReaderError(fmt::format(
                "daemon read timeout after {}s waiting for marker; tail:\n{}",
                timeout.count(), TrimLines(accumulator_, 250)));
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(daemonStdout_, nullptr, 0, nullptr, &avail, nullptr)) {
            throw BlueprintReaderError("daemon stdout pipe error");
        }
        if (avail == 0) {
            // Yield for a moment to avoid spinning.
            Sleep(15);
            continue;
        }
        DWORD toRead = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        DWORD got = 0;
        if (!ReadFile(daemonStdout_, buf, toRead, &got, nullptr) || got == 0) {
            throw BlueprintReaderError("daemon stdout closed");
        }
        accumulator_.append(buf, got);
    }
}

nlohmann::json CommandletBlueprintReader::RunOpDaemon(const std::vector<std::wstring>& opArgs) {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    EnsureDaemon();

    auto outFile = TempJsonPath();

    // Compose a single commandlet-arg line. Each arg is space-quoted as needed.
    std::wstring line;
    for (const auto& a : opArgs) {
        if (!line.empty()) line.push_back(L' ');
        line.append(QuoteArg(a));
    }
    line.append(L" -Out=" + QuoteArg(outFile.wstring()));
    line.append(L" -Compact\n");

    // RAII cleanup of the temp file: runs even if any throw between here
    // and the dismiss point at the end. Avoids the previous pattern of
    // manually `cleanup();` before each `throw` and relying on every
    // future caller to remember.
    auto outFileGuard = MakeScopeGuard([&]() {
        std::error_code ec;
        std::filesystem::remove(outFile, ec);
    });

    std::string lineUtf8 = Narrow(line);

    const auto t0 = std::chrono::steady_clock::now();

    // Loop on partial writes — anonymous pipes can in principle short-write
    // a long command line if the kernel pipe buffer is small. Today our
    // command lines are << pipe buffer size, but loop anyway for safety.
    {
        const char* p = lineUtf8.data();
        std::size_t left = lineUtf8.size();
        while (left > 0) {
            DWORD written = 0;
            if (!WriteFile(daemonStdin_, p,
                           static_cast<DWORD>(left), &written, nullptr) ||
                written == 0) {
                throw BlueprintReaderError("daemon stdin write failed");
            }
            p += written;
            left -= written;
        }
    }

    // Wait for the per-call sentinel `__BPR_DONE <code>__\n`. The plugin
    // emits this after every command. We discard everything else (engine
    // log lines on the merged stdout/stderr stream).
    int32_t exitCode = 0;
    {
        // Drain everything up to + including the leading marker. Per-call
        // timeout (cfg_.timeout) — the daemon is already warm at this point.
        ReadUntilMarker("__BPR_DONE ", cfg_.timeout);
        // The next bytes are decimal digits, then `__\n`. Read up to + including
        // the `__\n` terminator; the digits sit in `digits`.
        std::string digits = ReadUntilMarker("__\n", cfg_.timeout);
        try {
            exitCode = std::stoi(digits);
        } catch (...) {
            throw BlueprintReaderError(fmt::format(
                "daemon emitted malformed sentinel: code='{}'", digits));
        }
    }

    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr,
                 "[bp-reader-mcp][commandlet][daemon] op-args=%zu exit=%d duration=%lldms\n",
                 opArgs.size(), exitCode, static_cast<long long>(dt));

    if (exitCode != 0) {
        std::string tail = TrimLines(accumulator_, 250);
        if (exitCode == 4) {
            throw AssetNotFound(fmt::format(
                "daemon op exit=4 (missing target); tail:\n{}", tail));
        }
        throw BlueprintReaderError(fmt::format(
            "daemon op exit={}; tail:\n{}", exitCode, tail));
    }

    if (!std::filesystem::exists(outFile)) {
        throw BlueprintReaderError(fmt::format(
            "daemon op exited 0 but produced no output file at {}", outFile.string()));
    }

    nlohmann::json parsed;
    try {
        std::ifstream in(outFile);
        in >> parsed;
    } catch (const std::exception& e) {
        throw BlueprintReaderError(fmt::format(
            "failed to parse daemon JSON ({}): {}", outFile.string(), e.what()));
    }
    // outFileGuard runs here on normal return.
    return parsed;
}

#else // !_WIN32

void CommandletBlueprintReader::TerminateDaemon() {}
void CommandletBlueprintReader::EnsureDaemon() {
    throw BlueprintReaderError("daemon mode is Windows-only in Phase 1.5");
}
void CommandletBlueprintReader::Prewarm() {
    // No-op on non-Windows; daemon mode is unsupported there.
}
std::string CommandletBlueprintReader::ReadUntilMarker(const std::string&, std::chrono::seconds) {
    throw BlueprintReaderError("daemon mode is Windows-only in Phase 1.5");
}
nlohmann::json CommandletBlueprintReader::RunOpDaemon(const std::vector<std::wstring>&) {
    throw BlueprintReaderError("daemon mode is Windows-only in Phase 1.5");
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

std::string CommandletBlueprintReader::AddFunction(std::string_view assetPath,
                                                   std::string_view name) {
    std::vector<std::wstring> args;
    args.push_back(L"-Op=AddFunction");
    args.push_back(L"-Asset=" + Widen(assetPath));
    args.push_back(L"-Name="  + Widen(name));
    auto j = RunOp(args);
    if (j.is_object() && j.contains("function_name") && j["function_name"].is_string()) {
        return j["function_name"].get<std::string>();
    }
    return std::string(name);  // commandlet acked but didn't echo — caller passed in
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

} // namespace bpr::backends
