#include "Diagnostics.h"
#include "Env.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <regex>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

// Build version stamp (INSTALL-1): injected by CMakeLists.txt (installed-engine
// fallback) / BlueprintReaderMcpCore.Build.cs (UBT). Fall back if a build path
// forgets to set them.
#ifndef BPR_VERSION
#define BPR_VERSION "0.0.0"
#endif
#ifndef BPR_GIT_HASH
#define BPR_GIT_HASH "unknown"
#endif

namespace bpr::diag {

namespace diagnostics_detail {

Finding Ok(std::string label, std::string detail = {}) {
	return {Severity::Ok, std::move(label), std::move(detail), {}};
}
Finding Info(std::string label, std::string detail = {}) {
	return {Severity::Info, std::move(label), std::move(detail), {}};
}
Finding Warning(std::string label, std::string detail, std::string fix = {}) {
	return {Severity::Warning, std::move(label), std::move(detail), std::move(fix)};
}
Finding Error(std::string label, std::string detail, std::string fix = {}) {
	return {Severity::Error, std::move(label), std::move(detail), std::move(fix)};
}

// Walk up from the executable to find the plugin directory. Returns empty
// path if the standard layout doesn't apply (e.g. the user moved the exe).
std::filesystem::path GuessPluginDirFromCfg(const backends::BackendConfig& cfg) {
	// The fixturesDir defaults to <exeDir>/fixtures, so back-compute exeDir
	// from it. Plugin dir is 3 levels above exeDir:
	//   <projectRoot>/Binaries/Win64/BlueprintReaderMcp.exe
	auto exeDir = cfg.fixturesDir.parent_path();
	auto p = exeDir;
	for (int i = 0; i < 3 && !p.empty(); ++i)
	{
		p = p.parent_path();
	}
	return p;
}

// Locate the directory that actually contains BlueprintReader.uplugin. Unlike
// GuessPluginDirFromCfg (which yields the *project* root), this walks up from
// the exe and checks both "<dir>/BlueprintReader.uplugin" and the conventional
// "<dir>/Plugins/BlueprintReader/" nesting, so the version/git-staleness checks
// read the real plugin source. Empty if not found.
std::filesystem::path FindPluginDir(const backends::BackendConfig& cfg) {
	std::filesystem::path p = cfg.fixturesDir.parent_path();  // exeDir
	std::error_code ec;
	for (int i = 0; i < 6 && !p.empty(); ++i) {
		if (std::filesystem::is_regular_file(p / "BlueprintReader.uplugin", ec)) {
			return p;
		}
		auto nested = p / "Plugins" / "BlueprintReader";
		if (std::filesystem::is_regular_file(nested / "BlueprintReader.uplugin", ec)) {
			return nested;
		}
		p = p.parent_path();
	}
	return {};
}

// Best-effort: read VersionName from the plugin's .uplugin. Empty on failure.
std::string ReadUpluginVersion(const std::filesystem::path& pluginDir) {
	try {
		std::ifstream in(pluginDir / "BlueprintReader.uplugin");
		if (!in) {
			return {};
		}
		nlohmann::json j;
		in >> j;
		if (j.is_object()) {
			auto v = j.find("VersionName");
			if (v != j.end() && v->is_string()) {
				return v->get<std::string>();
			}
		}
	} catch (...) { /* best-effort */ }
	return {};
}

// Best-effort: the short git HEAD of a directory. Empty when the dir isn't a
// git repo, git isn't on PATH, or anything fails — staleness is then simply
// not reported (never blocks doctor).
std::string GitHeadShort(const std::filesystem::path& dir) {
#if defined(_WIN32)
	const char* devnull = "2>nul";
	const char* mode = "r";
	auto openPipe  = [](const char* c) { return _popen(c, "r"); };
	auto closePipe = [](FILE* f) { return _pclose(f); };
#else
	const char* devnull = "2>/dev/null";
	const char* mode = "r";
	(void)mode;
	auto openPipe  = [](const char* c) { return popen(c, "r"); };
	auto closePipe = [](FILE* f) { return pclose(f); };
#endif
	const std::string cmd =
		fmt::format("git -C \"{}\" rev-parse --short HEAD {}", dir.string(), devnull);
	FILE* p = openPipe(cmd.c_str());
	if (!p) {
		return {};
	}
	std::string out;
	char buf[128];
	while (std::fgets(buf, sizeof(buf), p)) {
		out += buf;
	}
	closePipe(p);
	while (!out.empty() &&
		   (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
		out.pop_back();
	}
	return out;
}

bool UProjectListsBlueprintReader(const std::filesystem::path& uproject) {
	try {
		std::ifstream in(uproject);
		if (!in)
		{
			return false;
		}
		nlohmann::json j;
		in >> j;
		if (!j.is_object())
		{
			return false;
		}
		auto plugins = j.find("Plugins");
		if (plugins == j.end() || !plugins->is_array())
		{
			return false;
		}
		for (const auto& p : *plugins) {
			if (!p.is_object())
			{
				continue;
			}
			auto name = p.find("Name");
			if (name != p.end() && name->is_string() &&
				name->get<std::string>() == "BlueprintReader") {
				auto enabled = p.find("Enabled");
				return enabled == p.end() ||
					   (enabled->is_boolean() && enabled->get<bool>());
			}
		}
	} catch (...) { /* fall through */ }
	return false;
}

}    // namespace diagnostics_detail
using namespace diagnostics_detail;

bool Report::HasError() const {
	for (const auto& f : findings)
	{
		if (f.severity == Severity::Error) return true;
	}
	return false;
}
bool Report::HasWarning() const {
	for (const auto& f : findings)
	{
		if (f.severity == Severity::Warning) return true;
	}
	return false;
}

Report RunSetupChecks(const backends::BackendConfig& cfg) {
	Report r;

	// -------- build version + staleness (INSTALL-1) --------
	// Always reported (both backends). Surfaces the exact build, and warns
	// when the running exe is older than the on-disk plugin source — the
	// failure mode behind the stale-copy build break.
	r.findings.push_back(Info(
		fmt::format("bp-reader-mcp build {}+{}", BPR_VERSION, BPR_GIT_HASH)));
	{
		auto pluginDir = FindPluginDir(cfg);
		if (!pluginDir.empty()) {
			const std::string srcVer = ReadUpluginVersion(pluginDir);
			if (!srcVer.empty() && srcVer != std::string(BPR_VERSION)) {
				r.findings.push_back(Warning(
					"server exe is stale vs the on-disk plugin (VersionName mismatch)",
					fmt::format("exe built from {}, plugin source is {}",
								BPR_VERSION, srcVer),
					"Rebuild the MCP server so the running exe matches the plugin "
					"source (Build-MCPServer.ps1 / build-mcp-cmake.ps1)."));
			}
			const std::string head = GitHeadShort(pluginDir);
			if (!head.empty() && std::string(BPR_GIT_HASH) != "unknown" &&
				head != std::string(BPR_GIT_HASH)) {
				r.findings.push_back(Warning(
					"server exe is stale vs the plugin's git HEAD",
					fmt::format("exe built from commit {}, plugin source is now at {}",
								BPR_GIT_HASH, head),
					"Rebuild the MCP server to pick up the latest plugin source."));
			}
		}
	}

	// -------- mock backend short-circuits the rest --------
	if (cfg.backend == "mock") {
		if (std::filesystem::is_directory(cfg.fixturesDir)) {
			r.findings.push_back(Ok("mock backend, fixtures directory present",
									cfg.fixturesDir.string()));
		} else {
			r.findings.push_back(Error(
				"fixtures directory missing", cfg.fixturesDir.string(),
				"Build the MCP server (cmake --build build --config Release) — "
				"the build copies fixtures next to the exe."));
		}
		return r;
	}

	// -------- commandlet backend: full setup chain --------

	// 1. Project file
	if (cfg.uproject.empty()) {
		r.findings.push_back(Error(
			"no project file (.uproject) configured", "",
			"Set BP_READER_PROJECT, or place the plugin under "
			"<project>/Plugins/BlueprintReader/ so auto-discovery finds the "
			"project."));
		return r;
	}
	if (!std::filesystem::is_regular_file(cfg.uproject)) {
		r.findings.push_back(Error(
			".uproject path doesn't exist", cfg.uproject.string(),
			"Check BP_READER_PROJECT (or the auto-discovered path)."));
		return r;
	}
	r.findings.push_back(Ok(".uproject file found", cfg.uproject.string()));

	// 2. .uproject lists the BlueprintReader plugin as enabled
	if (!UProjectListsBlueprintReader(cfg.uproject)) {
		r.findings.push_back(Warning(
			"BlueprintReader not in .uproject's Plugins[] array",
			"Auto-discovery (EnabledByDefault: true) usually handles this, "
			"but big-project TargetRules sometimes filter it out — adding "
			"an explicit entry is the reliable fix.",
			R"(Add to <YourProject>.uproject's Plugins array:
	{ "Name": "BlueprintReader", "Enabled": true })"));
	} else {
		r.findings.push_back(Ok("BlueprintReader is enabled in .uproject"));
	}

	// 3. Engine directory
	if (cfg.engineDir.empty()) {
		r.findings.push_back(Error(
			"engine directory not configured", "",
			"Set BP_READER_ENGINE_DIR, or right-click the .uproject and run "
			"'Switch Unreal Engine version' to register an EngineAssociation."));
		return r;
	}
	if (!std::filesystem::is_directory(cfg.engineDir)) {
		r.findings.push_back(Error(
			"engine directory doesn't exist", cfg.engineDir.string()));
		return r;
	}
	r.findings.push_back(Ok("engine directory found", cfg.engineDir.string()));

	// 3b. Engine version compatibility (INSTALL-3). The plugin is built/tested
	// against UE 5.7-5.8; warn (don't fail) outside that range so a mismatch
	// surfaces here instead of as a cryptic compile error. Best-effort parse
	// of Engine/Build/Build.version.
	{
		try {
			std::ifstream in(cfg.engineDir / "Engine" / "Build" / "Build.version");
			if (in) {
				nlohmann::json j;
				in >> j;
				if (j.is_object()) {
					const int major = j.value("MajorVersion", 0);
					const int minor = j.value("MinorVersion", 0);
					if (major != 0) {
						const std::string ver = fmt::format("{}.{}", major, minor);
						const bool supported = (major == 5 && minor >= 7 && minor <= 8);
						if (supported) {
							r.findings.push_back(Ok(
								"engine version in tested range (5.7-5.8)", ver));
						} else {
							r.findings.push_back(Warning(
								"engine version outside the tested range (5.7-5.8)", ver,
								"The plugin is built/tested against UE 5.7-5.8. Other "
								"versions may need source tweaks — guard new engine APIs "
								"with UE_VERSION_OLDER_THAN. Proceed with caution."));
						}
					}
				}
			}
		} catch (...) { /* best-effort — never block doctor on a parse error */ }
	}

	// 4. Editor commandlet exe matches the requested config
	auto binDir = cfg.engineDir / "Engine" / "Binaries" / "Win64";
	std::string cfgName = cfg.editorConfig.empty() ? "Development" : cfg.editorConfig;
	std::filesystem::path expectedExe = (cfgName == "Development")
		? binDir / "UnrealEditor-Cmd.exe"
		: binDir / fmt::format("UnrealEditor-Win64-{}-Cmd.exe", cfgName);
	if (!std::filesystem::is_regular_file(expectedExe)) {
		r.findings.push_back(Error(
			fmt::format("UnrealEditor-Cmd ({} config) not found", cfgName),
			expectedExe.string(),
			fmt::format(
				"Build the engine target in '{}' configuration, OR set "
				"BP_READER_EDITOR_CONFIG to a config you have built. "
				"(Common cause: studio ships only Development engine "
				"binaries; building the project in DebugGame doesn't "
				"produce UnrealEditor-Win64-DebugGame-Cmd.exe.)",
				cfgName)));
	} else {
		r.findings.push_back(Ok(
			fmt::format("UnrealEditor-Cmd ({} config) found", cfgName),
			expectedExe.string()));
	}

	// 5. BlueprintReaderEditor module DLL with matching config
	auto pluginDir = GuessPluginDirFromCfg(cfg);
	if (!pluginDir.empty()) {
		auto pluginBin = pluginDir / "Binaries" / "Win64";
		std::filesystem::path expectedDll = (cfgName == "Development")
			? pluginBin / "UnrealEditor-BlueprintReaderEditor.dll"
			: pluginBin / fmt::format(
				"UnrealEditor-BlueprintReaderEditor-Win64-{}.dll", cfgName);
		if (!std::filesystem::is_regular_file(expectedDll)) {
			// List anything that does exist to help the user diagnose.
			std::vector<std::string> have;
			std::error_code ec;
			if (std::filesystem::is_directory(pluginBin, ec)) {
				static const std::regex re(
					R"(^UnrealEditor-BlueprintReaderEditor(?:-Win64-([A-Za-z]+))?\.dll$)");
				for (const auto& e : std::filesystem::directory_iterator(pluginBin, ec)) {
					if (!e.is_regular_file(ec))
					{
						continue;
					}
					auto name = e.path().filename().string();
					std::smatch m;
					if (std::regex_match(name, m, re)) {
						have.push_back(m[1].matched ? m[1].str() : "Development");
					}
				}
			}
			std::string detail = expectedDll.string();
			std::string fix;
			if (have.empty()) {
				fix = fmt::format(
					"Build the plugin module:\n"
					"    Build.bat <Project>Editor Win64 {} \\\n"
					"        -project=\"{}\" \\\n"
					"        -Module=BlueprintReaderEditor",
					cfgName, cfg.uproject.string());
			} else {
				fix = fmt::format(
					"You have configs built: {}.\n"
					"Either set BP_READER_EDITOR_CONFIG=\"{}\" so the daemon "
					"uses a config you have, or build the missing variant "
					"with -Module=BlueprintReaderEditor in '{}' config.",
					fmt::join(have, ", "), have[0], cfgName);
			}
			r.findings.push_back(Error(
				fmt::format("UnrealEditor-BlueprintReaderEditor ({} config) "
							"DLL not built", cfgName),
				detail, fix));
		} else {
			r.findings.push_back(Ok(
				fmt::format("UnrealEditor-BlueprintReaderEditor ({} config) "
							"DLL found", cfgName),
				expectedDll.string()));
		}
	}

	return r;
}

void PrintReport(const Report& report, std::ostream& out, bool colors) {
	auto color = [&](Severity s) -> const char* {
		if (!colors)
		{
			return "";
		}
		switch (s) {
			case Severity::Ok:      return "\x1b[32m";  // green
			case Severity::Info:    return "\x1b[36m";  // cyan
			case Severity::Warning: return "\x1b[33m";  // yellow
			case Severity::Error:   return "\x1b[31m";  // red
		}
		return "";
	};
	auto reset = [&]() { return colors ? "\x1b[0m" : ""; };
	auto label = [](Severity s) {
		switch (s) {
			case Severity::Ok:      return "[ OK ]";
			case Severity::Info:    return "[INFO]";
			case Severity::Warning: return "[WARN]";
			case Severity::Error:   return "[FAIL]";
		}
		return "[????]";
	};
	for (const auto& f : report.findings) {
		out << color(f.severity) << label(f.severity) << reset() << " "
			<< f.label << "\n";
		if (!f.detail.empty())
		{
			out << "       " << f.detail << "\n";
		}
		if (!f.fix_hint.empty()) {
			// Indent multi-line fix hints.
			out << "       Fix: ";
			for (std::size_t i = 0; i < f.fix_hint.size(); ++i) {
				out << f.fix_hint[i];
				if (f.fix_hint[i] == '\n' && i + 1 < f.fix_hint.size()) {
					out << "       ";
				}
			}
			if (f.fix_hint.back() != '\n')
			{
				out << "\n";
			}
		}
	}
}

}    // namespace bpr::diag
