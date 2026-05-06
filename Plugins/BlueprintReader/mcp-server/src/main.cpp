// bp-reader-mcp — entry point.
//
// On startup:
//   1. Resolve executable directory to find the bundled fixtures dir.
//   2. Build the requested backend (env var BP_READER_BACKEND, default "mock").
//   3. Register MCP tools.
//   4. Run the JSON-RPC stdio loop until EOF.
//
// Diagnostics go to stderr; stdout is the JSON-RPC transport — never write
// anything else there.

#include "backends/BackendFactory.h"
#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>

#include <fmt/core.h>

#if defined(_WIN32)
    #include <io.h>
    #include <fcntl.h>
    #include <windows.h>
#endif

namespace {

std::filesystem::path ExecutableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

void EnsureBinaryStdio() {
#if defined(_WIN32)
    // Windows defaults stdin/stdout to text mode, which mangles framing
    // (CRLF translation, Ctrl-Z = EOF). Force binary.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Make the C++ streams unbuffered-ish so flushes show up immediately.
    std::cout.setf(std::ios::unitbuf);
}

} // namespace

int main() {
    EnsureBinaryStdio();

    using namespace bpr;

    auto exeDir = ExecutableDir();
    auto cfg = backends::ConfigFromEnv(exeDir);

    std::cerr << fmt::format(
        "[bp-reader-mcp] starting; backend={} fixtures={} engineDir={} uproject={} "
        "timeout={}s startupTimeout={}s daemon={} prewarm={}\n",
        cfg.backend, cfg.fixturesDir.string(),
        cfg.engineDir.string(), cfg.uproject.string(),
        cfg.timeoutSeconds, cfg.startupTimeoutSeconds,
        cfg.useDaemon ? "true" : "false",
        cfg.prewarm   ? "true" : "false");

    std::unique_ptr<backends::IBlueprintReader> reader;
    try {
        reader = backends::Create(cfg);
    } catch (const std::exception& e) {
        std::cerr << "[bp-reader-mcp] backend init failed: " << e.what() << "\n";
        return 1;
    }

    if (auto* mock = dynamic_cast<backends::MockBlueprintReader*>(reader.get())) {
        std::cerr << fmt::format("[bp-reader-mcp] loaded {} fixture(s)\n",
                                 mock->FixtureCount());
    }

    tools::ToolRegistry registry;
    tools::RegisterBlueprintTools(registry, *reader);

    jsonrpc::Server server;
    mcp::ServerInfo info;
    mcp::RegisterHandlers(server, registry, info);

    server.Run(std::cin, std::cout, std::cerr);
    std::cerr << "[bp-reader-mcp] stdin closed; exiting\n";
    return 0;
}
