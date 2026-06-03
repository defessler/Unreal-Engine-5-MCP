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
#include "jsonrpc/CallContext.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "jsonrpc/HttpServerMain.h"
#include "tools/Analytics.h"
#include "tools/BlueprintTools.h"
#include "tools/EditorSubscriptions.h"
#include "tools/Logger.h"
#include "tools/Prompts.h"
#include "tools/Resources.h"
#include "tools/ToolRegistry.h"
#include "tools/ToolsetMeta.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
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
  bp-reader-mcp version        Print the build version (VersionName + git hash).
  bp-reader-mcp doctor         Run setup checks; exit 0 if healthy.
  bp-reader-mcp config         Print a ready-to-paste MCP config snippet
							   with auto-discovered paths filled in.
  bp-reader-mcp config --client=claude-code     Same, formatted for Claude Code (.mcp.json).
  bp-reader-mcp config --client=claude-desktop  Same, for Claude Desktop config.
  bp-reader-mcp config --client=cursor          Same, for Cursor (.cursor/mcp.json).
  bp-reader-mcp config --client=windsurf        Same, for Windsurf (mcp_config.json).
  bp-reader-mcp config --client=copilot         Same, for VS Code Copilot (.vscode/mcp.json).
  bp-reader-mcp config --client=vscode          Alias of copilot (VS Code native MCP).
  bp-reader-mcp config --client=gemini          Same, for Gemini CLI (~/.gemini/settings.json).
  bp-reader-mcp config --client=codex           Same, for Codex (~/.codex/config.toml, TOML).
  bp-reader-mcp config --mock                   Emit a mock-backend-only snippet — no UE project
												or engine required. For fixture-backed testing
												of MCP client integrations.
  bp-reader-mcp dump-tools [--md|--json]  Print the full tool catalog (markdown table or JSON)
												from the registry — powers docs/TOOLS.md. Offline.
  bp-reader-mcp --help         This help.

Configuration is via environment variables — see README's "Configuration" section.
Most paths are auto-discovered from the exe's location; usually no env vars
are needed for the standard layout.
)";
}

// ---------------------------------------------------------------------------
// Build version stamp (INSTALL-1)
// ---------------------------------------------------------------------------
// BPR_VERSION / BPR_GIT_HASH are injected by the build (CMakeLists.txt for the
// installed-engine fallback, BlueprintReaderMcpCore.Build.cs for UBT). Fall
// back gracefully if a build path forgets to set them.
#ifndef BPR_VERSION
#define BPR_VERSION "0.0.0"
#endif
#ifndef BPR_GIT_HASH
#define BPR_GIT_HASH "unknown"
#endif
namespace {
std::string VersionString() {
	return std::string(BPR_VERSION) + "+" + BPR_GIT_HASH;
}
}    // namespace

// ---------------------------------------------------------------------------
// `doctor` subcommand
// ---------------------------------------------------------------------------

int RunDoctor() {
	auto exeDir = ExecutableDir();
	auto cfg = bpr::backends::ConfigFromEnv(exeDir, std::cerr);

	std::cout << "bp-reader-mcp doctor\n";
	std::cout << "  Version      : " << VersionString() << "\n";
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

	// Clients split into two config shapes:
	//   * `mcpServers` map — Claude Code (.mcp.json), Claude Desktop, Cursor
	//     (.cursor/mcp.json), Windsurf (~/.codeium/windsurf/mcp_config.json).
	//   * `servers` map with explicit "type":"stdio" — VS Code Copilot
	//     (.vscode/mcp.json) and VS Code's native MCP support.
	const char* placement = nullptr;
	if (client == "claude-code" || client == "claude-desktop" ||
		client == "cursor" || client == "windsurf" || client == "gemini") {
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
		if (client == "claude-code")    placement = "<project>/.mcp.json";
		else if (client == "claude-desktop") placement = "claude_desktop_config.json (Settings > Developer > Edit Config)";
		else if (client == "cursor")    placement = "<project>/.cursor/mcp.json (or ~/.cursor/mcp.json for global)";
		else if (client == "windsurf")  placement = "~/.codeium/windsurf/mcp_config.json";
		else if (client == "gemini")    placement = "~/.gemini/settings.json (or <project>/.gemini/settings.json)";
	} else if (client == "copilot" || client == "vscode") {
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
		placement = "<project>/.vscode/mcp.json";
	} else if (client == "codex") {
		// Codex uses TOML: a [mcp_servers.<name>] table + a nested .env table.
		// A JSON-escaped Windows path (\\) is also valid in a TOML basic
		// string, so we reuse exePath as-is inside the double quotes.
		std::string tomlEnv = mockMode
			? std::string(
R"([mcp_servers.bp-reader.env]
BP_READER_BACKEND = "mock"
)")
			: std::string(
R"([mcp_servers.bp-reader.env]
BP_READER_PREWARM = "1"
BP_READER_EDITOR_ARGS = "-EnableAllPlugins"
)");
		if (!mockMode && !cfgName.empty() && cfgName != "Development") {
			tomlEnv += fmt::format("BP_READER_EDITOR_CONFIG = \"{}\"\n", cfgName);
		}
		std::cout << fmt::format(
R"([mcp_servers.bp-reader]
command = "{exe}"
args = []

{env})", fmt::arg("exe", exePath), fmt::arg("env", tomlEnv));
		placement = "~/.codex/config.toml";
	} else {
		std::cerr << "config: unknown --client='" << client
				  << "' (try claude-code, claude-desktop, cursor, windsurf, "
					 "copilot, vscode, gemini, or codex)\n";
		return 1;
	}
	if (placement) {
		std::cerr << "\n# Place this in: " << placement << "\n";
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

	// Cooperative cancellation: wire the long one-shot subprocess wait loop to
	// the ambient per-call CallContext so a client's notifications/cancelled
	// kills a slow in-flight commandlet op (the fallback path when the daemon
	// is down). No-op when there's no active call or the client didn't cancel.
	cfg.cancelCheck = [] {
		auto* ctx = bpr::jsonrpc::CallContext::Current();
		return ctx != nullptr && ctx->IsCancelled();
	};

	// Relay mid-op progress frames (from the daemon, for long ops) to the
	// ambient call's notifications/progress. total<=0 means unknown-duration.
	cfg.progressSink = [](double cur, double total, const std::string& msg) {
		if (auto* ctx = bpr::jsonrpc::CallContext::Current()) {
			ctx->EmitProgress(cur,
							  total > 0.0 ? std::optional<double>(total) : std::nullopt,
							  msg);
		}
	};

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
	// Tool-search mode (BP_READER_TOOL_SEARCH=1) is the even-narrower
	// alternative to progressive disclosure; computed up front so
	// progressive can defer to it (mutually exclusive — tool search wins).
	const bool toolSearchMode =
		env::BoolOrDefault("BP_READER_TOOL_SEARCH", false, std::cerr);
	// Progressive disclosure is ON BY DEFAULT (opt out with
	// BP_READER_PROGRESSIVE=0). It advertises the ~35-tool `core` surface
	// plus the lazy-discovery meta-tools instead of the full ~252, cutting
	// the tools/list token cost dramatically. Agents reach the long tail
	// via `call_tool(name, arguments)` (no widening needed) or by widening
	// the advertised set with `enable_tool_category`.
	const bool progressiveMode =
		env::BoolOrDefault("BP_READER_PROGRESSIVE", true, std::cerr) && !toolSearchMode;
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

	// PARITY-4: dispatch-time governance allow/block (regex), distinct from the
	// disclosure filter above. Enforced by Find AND FindAny, so a blocked tool
	// can't be reached even via the lazy-discovery `call_tool` meta-tool.
	// Applies uniformly to every tool (incl. the transpile family) — the single
	// coverable governance knob for shared / untrusted multi-agent setups.
	// Malformed regex throws at startup (fail loud).
	auto toolAllow = splitCSV(env::GetOrDefault("BP_READER_TOOL_ALLOW"));
	auto toolBlock = splitCSV(env::GetOrDefault("BP_READER_TOOL_BLOCK"));
	if (!toolAllow.empty() || !toolBlock.empty()) {
		registry.ApplyGovernance(toolAllow, toolBlock);
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] governance: " << toolAllow.size()
					  << " allow + " << toolBlock.size()
					  << " block pattern(s) enforced at dispatch (incl. call_tool)\n";
		}
	}
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
		// Also surface the lazy-discovery meta-tools so the agent has a
		// `call_tool` escape hatch — dispatch ANY tool in the full surface
		// by name without first widening — plus list_toolsets /
		// describe_toolset for discovery. RegisterToolsetMetaTools is
		// idempotent, so this is safe whether or not they're already
		// registered as part of the main surface.
		tools::RegisterToolsetMetaTools(registry);
		registry.ActivateToken("list_toolsets");
		registry.ActivateToken("describe_toolset");
		registry.ActivateToken("call_tool");
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
	// Report the stamped build version on the initialize handshake so the
	// client/agent can see exactly which server build it's talking to.
	info.version = VersionString();
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
	// Phase 10 (EA-push) — opt-in editor push-event subscription model.
	// Off by default (BP_READER_PUSH_EVENTS=0): no editor capability is
	// advertised and editor/subscribe yields -32601. The Tier-A event
	// sources (editor delegates -> notifications) are a follow-up; this
	// wires the client-facing subscription surface.
	std::unique_ptr<tools::EditorSubscriptions> editorSubs;
	if (env::BoolOrDefault("BP_READER_PUSH_EVENTS", false, std::cerr)) {
		editorSubs = std::make_unique<tools::EditorSubscriptions>();
		if (env::VerboseLoggingEnabled()) {
			std::cerr << "[bp-reader-mcp] push events: editor/subscribe enabled\n";
		}
	}
	mcp::RegisterHandlers(server, registry, &promptRegistry, &logger,
						   &resources, info, editorSubs.get());

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
		const int rc = jsonrpc::http::RunHttpServer(server, httpPort, mcpPath, std::cerr,
													reader.get(), editorSubs.get());
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

// ---------------------------------------------------------------------------
// dump-tools — emit the full tool catalog (markdown table or JSON) from the
// ToolRegistry. Offline (mock backend; the backend is never invoked — only
// the registered descriptors are read), so it runs with no engine. Powers the
// generated docs/TOOLS.md + docs/tools.json: a single source of truth for tool
// counts/names that an offline AI (Copilot/Codex reading the repo) can consume
// cheaply instead of a full tools/list, and that kills count-drift across docs.
// ---------------------------------------------------------------------------
namespace {

std::string ToolCategoryOf(const std::string& desc) {
	// Descriptions begin with a "[category] " tag.
	if (!desc.empty() && desc.front() == '[') {
		const auto end = desc.find(']');
		if (end != std::string::npos) { return desc.substr(1, end - 1); }
	}
	return "other";
}

// First line of a description, minus the "[category]" tag, with table-hostile
// characters escaped and a length cap — for the markdown summary cell.
std::string ToolSummaryOf(const std::string& desc) {
	std::string s = desc;
	if (!s.empty() && s.front() == '[') {
		const auto e = s.find(']');
		if (e != std::string::npos) { s = s.substr(e + 1); }
	}
	if (const auto b = s.find_first_not_of(" \t"); b != std::string::npos) { s = s.substr(b); }
	if (const auto nl = s.find('\n'); nl != std::string::npos) { s = s.substr(0, nl); }
	std::string out;
	for (char c : s) { out += (c == '|') ? std::string("\\|") : std::string(1, c); }
	if (out.size() > 200) { out = out.substr(0, 197) + "..."; }
	return out;
}

}  // namespace

int RunDumpTools(const std::vector<std::string>& args) {
	using namespace bpr;
	bool asJson = false;
	for (const auto& a : args) {
		if (a == "--json") { asJson = true; }
		else if (a == "--md" || a == "--markdown") { asJson = false; }
	}

	// Build the full surface offline. RegisterBlueprintTools only stores the
	// descriptors (it captures the backend by reference; it never calls it
	// during registration), so the mock needs no engine and no fixtures —
	// point it at a dedicated EMPTY dir (not temp root, which is full of
	// non-fixture files the mock loader would choke on).
	tools::ToolRegistry registry;
	try {
		const auto emptyDir = std::filesystem::temp_directory_path() / "bpr-dumptools-empty";
		std::error_code ec;
		std::filesystem::create_directories(emptyDir, ec);
		backends::MockBlueprintReader reader(emptyDir);
		tools::RegisterBlueprintTools(registry, reader);
	} catch (const std::exception& e) {
		std::cerr << "dump-tools: failed to build tool registry: " << e.what() << "\n";
		return 1;
	}
	const auto& descs = registry.AllDescriptors();

	if (asJson) {
		nlohmann::json out = nlohmann::json::array();
		for (const auto& d : descs) {
			out.push_back({
				{"name",          d.name},
				{"category",      ToolCategoryOf(d.description)},
				{"description",   d.description},
				{"input_schema",  d.input_schema},
				{"output_schema", d.output_schema},
			});
		}
		std::cout << out.dump(2) << "\n";
		return 0;
	}

	std::map<std::string, std::vector<const tools::ToolDescriptor*>> byCat;
	for (const auto& d : descs) { byCat[ToolCategoryOf(d.description)].push_back(&d); }

	std::cout << "# BlueprintReader MCP — Tool Catalog\n\n";
	std::cout << "<!-- GENERATED by `BlueprintReaderMcp --dump-tools` — do not edit by hand.\n";
	std::cout << "     Regenerate: Plugins/BlueprintReader/Scripts/Dump-Tools.ps1 -->\n\n";
	std::cout << "**" << descs.size() << " tools** across " << byCat.size()
			  << " categories. Progressive disclosure advertises only a small `core` subset on "
				 "`tools/list` by default; use `enable_tool_category` / `call_tool` to reach the "
				 "rest (see [AGENTS.md](../AGENTS.md)). Full input/output schemas: `docs/tools.json`.\n\n";
	for (auto& [cat, list] : byCat) {
		std::sort(list.begin(), list.end(),
				  [](const tools::ToolDescriptor* a, const tools::ToolDescriptor* b) {
					  return a->name < b->name;
				  });
		std::cout << "## " << cat << "  (" << list.size() << ")\n\n";
		std::cout << "| Tool | Description |\n|------|-------------|\n";
		for (const auto* d : list) {
			std::cout << "| `" << d->name << "` | " << ToolSummaryOf(d->description) << " |\n";
		}
		std::cout << "\n";
	}
	return 0;
}

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
		if (a == "version" || a == "--version" || a == "-v") {
			std::cout << "bp-reader-mcp " << VersionString() << "\n";
			return 0;
		}
		if (a == "doctor") {
			return RunDoctor();
		}
		if (a == "config") {
			return RunConfig({args.begin() + 1, args.end()});
		}
		if (a == "dump-tools" || a == "--dump-tools") {
			return RunDumpTools({args.begin() + 1, args.end()});
		}
		// Unknown subcommand — print usage on stderr and bail.
		std::cerr << "bp-reader-mcp: unknown argument '" << a << "'\n\n";
		PrintUsage(std::cerr);
		return 2;
	}

	return RunServerLoop();
}
