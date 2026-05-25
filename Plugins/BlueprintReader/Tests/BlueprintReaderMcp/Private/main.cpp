// bp-reader-mcp — entry point.
//
// Without subcommand args: runs the MCP JSON-RPC stdio loop (the normal
// production path used by Claude Code, Copilot, etc.).
//
// With subcommands:
//   bp-reader-mcp.exe doctor              — run setup checks, exit non-zero
//                                           if anything's broken. Replaces
//                                           Verify-Build.bat for live diagnosis.
//   bp-reader-mcp.exe config [--client]   — print a ready-to-paste MCP
//                                           config snippet using the
//                                           auto-discovered paths.
//   bp-reader-mcp.exe --help              — usage.
//
// Diagnostics + subcommand output go to stderr/stdout-text. The MCP loop's
// stdout is the JSON-RPC transport — never write anything non-framed there.

#include "backends/BackendFactory.h"
#include "backends/MockBlueprintReader.h"
#include "Diagnostics.h"
#include "Env.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "jsonrpc/HttpServerMain.h"
#include "tools/Analytics.h"
#include "tools/BlueprintTools.h"
#include "tools/Logger.h"
#include "tools/Prompts.h"
#include "tools/Resources.h"
#include "tools/ToolRegistry.h"
#include "tools/ToolsetMeta.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#if defined(_WIN32)
	#include <fcntl.h>
	#include <io.h>
	#include <windows.h>
#endif    // defined(_WIN32)

namespace main_detail {

std::filesystem::path ExecutableDir() {
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n == MAX_PATH) {
		return std::filesystem::current_path();
	}
	return std::filesystem::path(buf).parent_path();
#else    // defined(_WIN32)
	return std::filesystem::current_path();
#endif    // defined(_WIN32)
}

std::filesystem::path ExecutablePath() {
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n == MAX_PATH) return {};
	return std::filesystem::path(buf);
#else    // defined(_WIN32)
	return {};
#endif    // defined(_WIN32)
}

void EnsureBinaryStdio() {
#if defined(_WIN32)
	// Windows defaults stdin/stdout to text mode, which mangles framing
	// (CRLF translation, Ctrl-Z = EOF). Force binary.
	_setmode(_fileno(stdin),  _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif    // defined(_WIN32)
	// Two layers of buffering on Windows: iostream's filebuf and the C
	// runtime's stdout buffer. `unitbuf` makes the iostream layer flush on
	// every <<, but the bytes can still sit in the CRT block buffer until it
	// fills up — for an MCP client reading bytes off a pipe, that means the
	// response sits in our CRT buffer until the next request arrives, and
	// the client's handshake timeout fires. Disable CRT buffering so every
	// WriteFrame() actually hits the pipe immediately.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::cout.setf(std::ios::unitbuf);
}

void PrintUsage(std::ostream& out) {
	out <<
R"(bp-reader-mcp — MCP server exposing UE5 BlueprintReader tools.

Usage:
  bp-reader-mcp                Run the JSON-RPC stdio loop (production mode).
  bp-reader-mcp doctor         Run setup checks; exit 0 if healthy.
  bp-reader-mcp config         Print a ready-to-paste MCP config snippet
							   with auto-discovered paths filled in.
  bp-reader-mcp config --client=claude-code     Same, formatted for Claude Code (.mcp.json).
  bp-reader-mcp config --client=claude-desktop  Same, for Claude Desktop config.
  bp-reader-mcp config --client=copilot         Same, for VS Code Copilot (.vscode/mcp.json).
  bp-reader-mcp config --mock                   Emit a mock-backend-only snippet — no UE project
												or engine required. For fixture-backed testing
												of MCP client integrations.
  bp-reader-mcp --help         This help.

Configuration is via environment variables — see README's "Configuration" section.
Most paths are auto-discovered from the exe's location; usually no env vars
are needed for the standard layout.
)";
}

// ---------------------------------------------------------------------------
// `doctor` subcommand
// ---------------------------------------------------------------------------

int RunDoctor() {
	auto exeDir = ExecutableDir();
	auto cfg = bpr::backends::ConfigFromEnv(exeDir, std::cerr);

	std::cout << "bp-reader-mcp doctor\n";
	std::cout << "  Exe          : " << ExecutablePath().string() << "\n";
	std::cout << "  Backend      : " << cfg.backend << "\n";
	std::cout << "  Engine       : "
			  << (cfg.engineDir.empty() ? "(not configured)" : cfg.engineDir.string())
			  << "\n";
	std::cout << "  Project      : "
			  << (cfg.uproject.empty() ? "(not configured)" : cfg.uproject.string())
			  << "\n";
	std::cout << "  EditorConfig : "
			  << (cfg.editorConfig.empty() ? "Development (default)" : cfg.editorConfig)
			  << "\n\n";

	auto report = bpr::diag::RunSetupChecks(cfg);
	bpr::diag::PrintReport(report, std::cout, /*colors=*/true);

	if (report.HasError()) {
		std::cout << "\nResult: \x1b[31mFAIL\x1b[0m — fix the errors above and re-run.\n";
		return 1;
	}
	if (report.HasWarning()) {
		std::cout << "\nResult: \x1b[33mOK with warnings\x1b[0m — see above.\n";
		return 0;
	}
	std::cout << "\nResult: \x1b[32mOK\x1b[0m — everything looks good.\n";
	return 0;
}

// ---------------------------------------------------------------------------
// `config` subcommand
// ---------------------------------------------------------------------------

std::string EscapeJsonPath(const std::filesystem::path& p) {
	std::string s = p.string();
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		if (c == '\\')
		{
			out.append("\\\\");
		}
		else out.push_back(c);
	}
	return out;
}

int RunConfig(const std::vector<std::string>& args) {
	std::string client = "claude-code";  // default
	bool mockMode = false;  // --mock: emit a snippet for the mock backend,
							// skipping the UE-project / engine discovery
							// requirement entirely. For users who just want
							// to wire bp-reader-mcp.exe into Claude Code /
							// Copilot for fixture-backed testing without a
							// running editor.
	for (const auto& a : args) {
		if (a.rfind("--client=", 0) == 0) {
			client = a.substr(9);
		} else if (a == "--mock") {
			mockMode = true;
		} else if (a == "--help" || a == "-h") {
			PrintUsage(std::cout);
			return 0;
		}
	}

	auto exeDir = ExecutableDir();
	std::ostringstream notes;
	auto cfg = bpr::backends::ConfigFromEnv(exeDir, notes);

	if (!mockMode) {
		if (cfg.backend != "commandlet") {
			std::cerr << "config: backend is '" << cfg.backend
					  << "' (no UE project discovered). Either drop the plugin "
						 "into <project>/Plugins/BlueprintReader/ + build the "
						 "Mcp target so the exe lands at <project>/Binaries/"
						 "Win64/BlueprintReaderMcp.exe, set BP_READER_PROJECT, "
						 "or pass --mock for a mock-backend-only snippet.\n";
			return 1;
		}
		if (cfg.uproject.empty() || cfg.engineDir.empty()) {
			std::cerr << "config: auto-discovery couldn't find both a .uproject "
						 "and an engine path. Set BP_READER_PROJECT / "
						 "BP_READER_ENGINE_DIR and re-run, or pass --mock for a "
						 "mock-backend-only snippet, or run `bp-reader-mcp "
						 "doctor` for details.\n";
			return 1;
		}
	}

	auto exePath = EscapeJsonPath(ExecutablePath());
	auto enginePath = EscapeJsonPath(cfg.engineDir);
	auto projPath = EscapeJsonPath(cfg.uproject);
	std::string cfgName = cfg.editorConfig.empty() ? "" : cfg.editorConfig;

	// Mock-mode emits a minimal env block: just the backend selector. The
	// mock backend's fixtures dir auto-resolves to `<exe>/fixtures` so no
	// path config is needed. Everything else in the live-backend block
	// (prewarm, editor args, editor config) is irrelevant to mock.
	std::string envBlock = mockMode
		? std::string(
R"(      "env": {
		"BP_READER_BACKEND": "mock"
	  })")
		: fmt::format(
R"(      "env": {{
		"BP_READER_PREWARM":       "1",
		"BP_READER_EDITOR_ARGS":   "-EnableAllPlugins"{cfgLine}
	  }})",
			fmt::arg("cfgLine", (cfgName.empty() || cfgName == "Development")
									? ""
									: fmt::format(",\n        \"BP_READER_EDITOR_CONFIG\": \"{}\"", cfgName)));

	if (client == "claude-code" || client == "claude-desktop") {
		std::cout << fmt::format(
R"({{
  "mcpServers": {{
	"bp-reader": {{
	  "command": "{exe}",
{env}
	}}
  }}
}}
)", fmt::arg("exe", exePath), fmt::arg("env", envBlock));
	} else if (client == "copilot") {
		std::cout << fmt::format(
R"({{
  "servers": {{
	"bp-reader": {{
	  "type": "stdio",
	  "command": "{exe}",
{env}
	}}
  }}
}}
)", fmt::arg("exe", exePath), fmt::arg("env", envBlock));
	} else {
		std::cerr << "config: unknown --client='" << client
				  << "' (try claude-code, claude-desktop, or copilot)\n";
		return 1;
	}

	if (mockMode) {
		std::cerr << "\n# Mock-backend mode: no UE project or engine required.\n"
					 "# Fixtures resolve to <exe>/fixtures (staged automatically\n"
					 "# at build time). Use this snippet to wire your MCP client\n"
					 "# for fixture-backed testing — production / live BP work\n"
					 "# needs the plugin built into a real UE project (drop the\n"
					 "# `\"BP_READER_BACKEND\": \"mock\"` line and re-run `config`\n"
					 "# from there for the production snippet).\n";
	} else {
		std::cerr << "\n# Auto-discovery notes:\n";
		std::cerr << notes.str();
		std::cerr << "\n# Engine: " << cfg.engineDir.string()
				  << "\n# Project: " << cfg.uproject.string()
				  << "\n# EditorConfig: "
				  << (cfgName.empty() ? "Development (default)" : cfgName)
				  << "\n# Auto-discovered values are NOT echoed in the env block "
					 "above —\n# the server picks them up from the exe location at "
					 "runtime.\n# Override with explicit BP_READER_* vars if you "
					 "need to.\n";
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Production MCP loop with startup sanity check
// ---------------------------------------------------------------------------

int RunServerLoop() {
	using namespace bpr;
	EnsureBinaryStdio();

	auto exeDir = ExecutableDir();
	auto cfg = backends::ConfigFromEnv(exeDir, std::cerr);

	// Note: single-instance enforcement used to live here, but the
	// commandlet daemon now owns its own per-project lock at
	// <Project>/Saved/bp-reader-cmdlet.lock. Multiple MCP clients
	// (Claude Code + Claude Desktop + Copilot) can now each run their
	// own bp-reader-mcp.exe against the same project and share a
	// single daemon over TCP via SocketBlueprintReader — see
	// feat/multi-session-shared-daemon.

	if (env::VerboseLoggingEnabled()) {
		std::cerr << fmt::format(
			"[bp-reader-mcp] starting; backend={} fixtures={} engineDir={} uproject={} "
			"timeout={}s startupTimeout={}s daemon={} prewarm={} editorConfig={} editorArgs=\"{}\"\n",
			cfg.backend, cfg.fixturesDir.string(),
			cfg.engineDir.string(), cfg.uproject.string(),
			cfg.timeoutSeconds, cfg.startupTimeoutSeconds,
			cfg.useDaemon ? "true" : "false",
			cfg.prewarm   ? "true" : "false",
			cfg.editorConfig.empty() ? "Development" : cfg.editorConfig,
			cfg.editorExtraArgs);
	}

	// Run setup checks BEFORE we attempt to spin up the daemon. Any
	// findings get logged immediately so users see the actionable hint
	// instead of silent hang / cryptic "daemon exited" 30 s later.
	auto report = diag::RunSetupChecks(cfg);
	if (!report.findings.empty()) {
		std::cerr << "[bp-reader-mcp] setup check:\n";
		diag::PrintReport(report, std::cerr, /*colors=*/false);
	}

	std::unique_ptr<backends::IBlueprintReader> reader;
	try {
		reader = backends::Create(cfg);
	} catch (const std::exception& e) {
		std::cerr << "[bp-reader-mcp] backend init failed: " << e.what() << "\n";
		return 1;
	}

	if (auto* mock = dynamic_cast<backends::MockBlueprintReader*>(reader.get())) {
		if (env::VerboseLoggingEnabled()) {
			std::cerr << fmt::format("[bp-reader-mcp] loaded {} fixture(s)\n",
									 mock->FixtureCount());
		}
	}

	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, *reader);

	// Optional env-var filter. Lets users with tool-count-capped MCP
	// clients (GitHub Copilot caps at 128 tools across all servers +
	// built-ins) pare the surface down to what they need. Both vars
	// accept comma-separated tokens that are either tool names or
	// category names (see tools/ToolCategories.cpp for the list).
	//   BP_READER_TOOLS         allow-list; empty = "all tools"
	//   BP_READER_TOOLS_EXCLUDE deny-list; subtracted after the allow step
	auto splitCSV = [](const std::string& s) {
		std::vector<std::string> out;
		std::string cur;
		for (char c : s) {
			if (c == ',' || c == ';' || c == ' ') {
				if (!cur.empty()) { out.push_back(cur); cur.clear(); }
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty())
		{
			out.push_back(cur);
		}
		return out;
	};

	// Progressive disclosure (BP_READER_PROGRESSIVE=1). Start with an
	// initial narrow set (BP_READER_TOOLS_INITIAL, default `core`) and
	// expose a meta-tool the agent calls to widen the surface mid-
	// session. The meta-tool's handler activates more categories and
	// sets a flag the MCP dispatcher reads to emit
	// `notifications/tools/list_changed`. Each call narrows the
	// surface gap; the agent never sees the full 119-tool list up
	// front. Compatible with BP_READER_TOOLS — if both are set, the
	// static filter wins (progressive mode then operates against the
	// statically-filtered superset).
	const bool progressiveMode =
		env::BoolOrDefault("BP_READER_PROGRESSIVE", false, std::cerr);
	std::vector<std::string> allowSpec =
		splitCSV(env::GetOrDefault("BP_READER_TOOLS"));
	std::vector<std::string> denySpec =
		splitCSV(env::GetOrDefault("BP_READER_TOOLS_EXCLUDE"));
	if (progressiveMode && allowSpec.empty()) {
		// No explicit BP_READER_TOOLS — use the progressive initial set.
		allowSpec = splitCSV(
			env::GetOrDefault("BP_READER_TOOLS_INITIAL", "core"));
	}
	// Per-backend capability filter — drop tools the active backend
	// can't fulfill so the catalog doesn't advertise dead surface.
	// Mock backend in particular has no editor / asset registry and
	// throws "not supported by this backend" for a long tail of ops;
	// hiding those means agents don't burn turns discovering the gap.
	auto backendDeny = reader->UnsupportedTools();
	if (!backendDeny.empty()) {
		denySpec.insert(denySpec.end(),
						std::make_move_iterator(backendDeny.begin()),
						std::make_move_iterator(backendDeny.end()));
	}
	const size_t before = registry.Size();
	registry.ApplyFilter(allowSpec, denySpec);
	if ((!allowSpec.empty() || !denySpec.empty()) && env::VerboseLoggingEnabled()) {
		std::cerr << "[bp-reader-mcp] tool filter: kept "
				  << registry.Size() << " of " << before
				  << " tools";
		if (!allowSpec.empty())
		{
			std::cerr << " (allow=" << env::GetOrDefault("BP_READER_TOOLS") << ")";
		}
		if (!denySpec.empty())
		{
			std::cerr << " (deny="  << env::GetOrDefault("BP_READER_TOOLS_EXCLUDE") << ")";
		}
		std::cerr << "\n";
	}
	if (progressiveMode) {
		tools::RegisterProgressiveDisclosureMetaTool(registry);
		// Filter has already been applied — Add() leaves the new tool
		// INACTIVE so callers don't see surprise surface widening.
		// Explicitly activate the meta-tool so the agent can use it to
		// widen the surface themselves.
		registry.ActivateToken("enable_tool_category");
		// Clear the list-changed flag set by ActivateToken — there's no
		// client connected yet to receive a notification.
		registry.TakeListChangedFlag();
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] progressive disclosure: enabled. "
					  << "Initial active set is " << registry.Size()
					  << " tools (of " << registry.TotalRegistered()
					  << " registered). Agent can widen via "
						 "`enable_tool_category(<name>)`.\n";
		}
	}

	// Lazy tool discovery (BP_READER_TOOL_SEARCH=1). Trim the advertised
	// tools/list down to just 3 meta-tools — list_toolsets,
	// describe_toolset, call_tool — and dispatch everything else through
	// call_tool. Massive context savings for clients that pay per
	// tools/list token: agents see 3 schemas up front instead of ~127,
	// and expand only the toolset they need this turn.
	//
	// Mutually exclusive with progressive disclosure — they're competing
	// models for the same problem. If both are set, tool search wins
	// because its surface is even narrower.
	const bool toolSearchMode =
		env::BoolOrDefault("BP_READER_TOOL_SEARCH", false, std::cerr);
	if (toolSearchMode) {
		tools::RegisterToolsetMetaTools(registry);
		tools::EnableToolSearchMode(registry);
		registry.TakeListChangedFlag();  // no client connected yet
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] tool search: enabled. "
					  << "Advertising " << registry.Size()
					  << " meta-tool(s) (of " << registry.TotalRegistered()
					  << " registered). Agent dispatches via "
						 "`call_tool(name, arguments)`.\n";
		}
	}

	// Pre-flight: refuse to run with an empty advertised tool surface.
	// If the static filter (BP_READER_TOOLS) was too aggressive — or if
	// no real tools registered at all (a bug in RegisterBlueprintTools or
	// a backend rejection that hid all tools) — we'd otherwise present
	// the client with an empty tools/list and look broken. Fail loud at
	// startup with an actionable hint instead.
	if (!registry.HasValidTools()) {
		std::cerr << "[bp-reader-mcp] startup: no tools are active "
				  << "(registered=" << registry.TotalRegistered()
				  << "). Check BP_READER_TOOLS / BP_READER_TOOLS_EXCLUDE "
					 "env vars — the filter spec may not match any tool. "
					 "Set BP_READER_TOOLS= (empty) to clear it.\n";
		return 2;
	}

	jsonrpc::Server server;
	mcp::ServerInfo info;
	// Ship the onboarding `instructions` text by default. Clients on the
	// MCP spec read this once at session start as system-prompt context.
	// Set BP_READER_INSTRUCTIONS=0 to suppress when the client supplies
	// its own system prompt and you don't want to spend tokens on ours.
	if (env::BoolOrDefault("BP_READER_INSTRUCTIONS", true, std::cerr)) {
		info.instructions = mcp::DefaultInstructions();
	}

	// Phase 3 — register the 8 built-in slash-command prompts unless
	// the user opted out. Empty registry → prompts capability is NOT
	// advertised on initialize, so older clients see the same surface
	// as before this commit.
	tools::prompts::PromptRegistry promptRegistry;
	if (env::BoolOrDefault("BP_READER_PROMPTS", true, std::cerr)) {
		tools::prompts::RegisterBuiltinPrompts(promptRegistry);
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] prompts: registered "
					  << promptRegistry.Size() << " built-in slash commands\n";
		}
	}

	// Phase 6 — Logger advertises `logging` capability + wires the
	// `logging/setLevel` handler. Default level is `info`. Set
	// BP_READER_LOG_LEVEL to override at startup (debug/info/notice/
	// warning/error/critical/alert/emergency/off). Setting to `off`
	// suppresses notifications/message entirely; stderr stays
	// unaffected.
	tools::Logger logger(&server);
	{
		const std::string startupLevel =
			env::GetOrDefault("BP_READER_LOG_LEVEL", "info");
		if (!logger.SetLevelFromString(startupLevel)) {
			std::cerr << "[bp-reader-mcp] warning: BP_READER_LOG_LEVEL='"
					  << startupLevel
					  << "' is not recognized; defaulting to info\n";
			logger.SetLevel(tools::LogLevel::Info);
		}
	}
	// Phase 4 — Resources primitive. Three bp:// providers wired by
	// default; BP_READER_RESOURCES=0 suppresses (empty registry →
	// resources capability not advertised, methods not registered).
	tools::resources::ResourceRegistry resources;
	if (env::BoolOrDefault("BP_READER_RESOURCES", true, std::cerr)) {
		resources.Add(tools::resources::MakeBlueprintAssetProvider(*reader));
		resources.Add(tools::resources::MakeProjectMetadataProvider(*reader));
		resources.Add(tools::resources::MakeOutputLogProvider(*reader));
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] resources: registered "
					  << resources.ProviderCount() << " bp:// providers\n";
		}
	}
	mcp::RegisterHandlers(server, registry, &promptRegistry, &logger,
						   &resources, info);

	// Phase 7 — Analytics + EULA. Both default to "minimum surface":
	// the no-op provider does nothing; the EULA notice fires once on
	// startup so the user has been told what data flows where. The
	// notice is suppressed under BP_READER_QUIET_STARTUP=1 for CI use.
	auto analytics = tools::MakeNoOpAnalyticsProvider();
	analytics->OnSessionStart();
	if (!env::BoolOrDefault("BP_READER_QUIET_STARTUP", false, std::cerr)) {
		std::cerr << tools::EulaNotice();
	}
	if (tools::AnalyticsEnabled() && env::VerboseLoggingEnabled()) {
		std::cerr << "[bp-reader-mcp] analytics: enabled (no-op default provider — "
					 "replace via future BP_READER_ANALYTICS_PROVIDER hook)\n";
	}
	struct AnalyticsLifecycle {
		tools::IAnalyticsProvider* p;
		~AnalyticsLifecycle() { if (p) p->OnSessionEnd(); }
	} lifecycle{analytics.get()};

	// Phase 9 (C3) — opt-in HTTP transport. When BP_READER_HTTP_PORT is
	// set, serve JSON-RPC over a localhost HTTP socket loop instead of
	// stdio (stdio remains the default; this branch is purely additive).
	const std::string httpPortStr = env::GetOrDefault("BP_READER_HTTP_PORT", "");
	if (!httpPortStr.empty()) {
		const uint16_t httpPort = static_cast<uint16_t>(std::strtoul(httpPortStr.c_str(), nullptr, 10));
		const std::string mcpPath = env::GetOrDefault("BP_READER_HTTP_PATH", "/mcp");
		std::cerr << "[bp-reader-mcp] HTTP transport on 127.0.0.1:" << httpPort
				  << mcpPath << " (stdio disabled)\n";
		const int rc = jsonrpc::http::RunHttpServer(server, httpPort, mcpPath, std::cerr);
		return rc;
	}

	server.Run(std::cin, std::cout, std::cerr);
	if (env::VerboseLoggingEnabled()) {
		std::cerr << "[bp-reader-mcp] stdin closed; exiting\n";
	}
	return 0;
}

}    // namespace main_detail
using namespace main_detail;

int main(int argc, char** argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i)
	{
		args.emplace_back(argv[i]);
	}

	if (!args.empty()) {
		const auto& a = args[0];
		if (a == "--help" || a == "-h" || a == "help") {
			PrintUsage(std::cout);
			return 0;
		}
		if (a == "doctor") {
			return RunDoctor();
		}
		if (a == "config") {
			return RunConfig({args.begin() + 1, args.end()});
		}
		// Unknown subcommand — print usage on stderr and bail.
		std::cerr << "bp-reader-mcp: unknown argument '" << a << "'\n\n";
		PrintUsage(std::cerr);
		return 2;
	}

	return RunServerLoop();
}
