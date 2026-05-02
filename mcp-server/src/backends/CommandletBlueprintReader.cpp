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

struct ProcResult {
    bool launched = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string stdoutTail;
    std::string stderrTail;
    std::string failureReason;
};

void AppendTail(std::string& tail, const char* buf, size_t n, size_t cap = 8192) {
    tail.append(buf, n);
    if (tail.size() > cap) {
        tail.erase(0, tail.size() - cap);
    }
}

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

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
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

    editorCmdExe_ = cfg_.engineDir / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe";
    if (!std::filesystem::exists(editorCmdExe_)) {
        throw BlueprintReaderError(fmt::format(
            "UnrealEditor-Cmd.exe not found at: {}", editorCmdExe_.string()));
    }
    if (!std::filesystem::exists(cfg_.uproject)) {
        throw BlueprintReaderError(fmt::format(
            "uproject not found at: {}", cfg_.uproject.string()));
    }
}

nlohmann::json CommandletBlueprintReader::RunOp(const std::vector<std::wstring>& opArgs) {
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

    const auto t0 = std::chrono::steady_clock::now();
    auto r = RunChild(editorCmdExe_.wstring(), args, cfg_.timeout);
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Best-effort log to stderr.
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
            cfg_.timeout.count(), TrimLines(r.stderrTail, 50)));
    }
    if (r.exitCode != 0) {
        // Exit code 4 from our commandlet means "asset/graph/function not found";
        // surface that as AssetNotFound for the MCP layer.
        std::string tail = TrimLines(r.stderrTail.empty() ? r.stdoutTail : r.stderrTail, 50);
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

std::vector<BPNode> CommandletBlueprintReader::FindNode(std::string_view assetPath, std::string_view query) {
    auto j = RunOp({
        L"-Op=Find",
        L"-Asset=" + Widen(assetPath),
        L"-Query=" + Widen(query),
    });
    return j.get<std::vector<BPNode>>();
}

} // namespace bpr::backends
