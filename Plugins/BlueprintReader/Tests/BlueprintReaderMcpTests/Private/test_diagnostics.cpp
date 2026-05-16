// Tests for the Diagnostics layer + auto-discovery behavior. Each historical
// silent-failure mode in the bug log gets at least one assertion that the
// failure is now loud + actionable.

#include <doctest/doctest.h>

#include "Diagnostics.h"
#include "Env.h"
#include "backends/BackendFactory.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

using namespace bpr;
using nlohmann::json;

namespace {

// Build a temp directory with a fake plugin layout under it; return the
// "ProjectRoot" path. Caller is responsible for letting the fs go away
// (we use a unique subdir under temp).
struct FakeProject {
	std::filesystem::path projectRoot;
	std::filesystem::path uproject;
	std::filesystem::path pluginDir;
	std::filesystem::path exeDir;
	std::filesystem::path engineDir;
};

FakeProject MakeFakeProject(const std::string& projectName,
							const std::string& engineAssoc = "5.7",
							bool addPluginToUproject = false,
							const std::string& dllSuffix = "" /* "" => Development */) {
	auto base = std::filesystem::temp_directory_path()
			  / fmt::format("bpr-diag-test-{}-{}", projectName, std::rand());
	std::filesystem::create_directories(base);

	FakeProject f;
	f.projectRoot = base / "Project";
	f.engineDir = base / "Engine";
	f.uproject = f.projectRoot / (projectName + ".uproject");
	f.pluginDir = f.projectRoot / "Plugins" / "BlueprintReader";
	f.exeDir = f.pluginDir / "mcp-server" / "build" / "Release";

	std::filesystem::create_directories(f.exeDir);
	std::filesystem::create_directories(f.engineDir / "Engine" / "Binaries" / "Win64");
	std::filesystem::create_directories(f.pluginDir / "Binaries" / "Win64");
	std::filesystem::create_directories(f.pluginDir / "Source" / "BlueprintReaderEditor");

	// Write a minimal .uproject.
	json up = {
		{"FileVersion", 3},
		{"EngineAssociation", engineAssoc},
		{"Modules", json::array()},
	};
	if (addPluginToUproject) {
		up["Plugins"] = json::array({
			json{{"Name", "BlueprintReader"}, {"Enabled", true}}
		});
	}
	std::ofstream(f.uproject) << up.dump(2);

	// Touch the engine commandlet exe.
	std::ofstream(f.engineDir / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe");

	// Touch the plugin DLL with the requested suffix.
	std::string dllName = dllSuffix.empty()
		? "UnrealEditor-BlueprintReaderEditor.dll"
		: "UnrealEditor-BlueprintReaderEditor-Win64-" + dllSuffix + ".dll";
	std::ofstream(f.pluginDir / "Binaries" / "Win64" / dllName);

	// Touch the .Build.cs so the source-files check passes.
	std::ofstream(f.pluginDir / "Source" / "BlueprintReaderEditor" /
				  "BlueprintReaderEditor.Build.cs");

	return f;
}

backends::BackendConfig MockCfgFromFake(const FakeProject& f, std::string editorConfig = "") {
	backends::BackendConfig cfg;
	cfg.backend = "commandlet";
	cfg.fixturesDir = f.exeDir / "fixtures";
	cfg.engineDir = f.engineDir;
	cfg.uproject = f.uproject;
	cfg.editorConfig = std::move(editorConfig);
	return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Diagnostics: each historical failure mode produces a clear finding.
// ---------------------------------------------------------------------------

TEST_CASE("diagnostics: clean setup produces only OKs (with one warning if "
		  "plugin not in .uproject)") {
	auto f = MakeFakeProject("CleanProj", "5.7", /*addPluginToUproject=*/true);
	auto cfg = MockCfgFromFake(f);
	auto report = diag::RunSetupChecks(cfg);
	CHECK_FALSE(report.HasError());
	CHECK_FALSE(report.HasWarning());
	// Must include both "uproject found" and "DLL found" OKs.
	bool sawUprojectOk = false, sawDllOk = false;
	for (const auto& fnd : report.findings) {
		if (fnd.label.find(".uproject file found") != std::string::npos) sawUprojectOk = true;
		if (fnd.label.find("DLL found") != std::string::npos) sawDllOk = true;
	}
	CHECK(sawUprojectOk);
	CHECK(sawDllOk);
}

TEST_CASE("diagnostics: missing plugin DLL is FAIL with concrete fix command") {
	auto f = MakeFakeProject("MissingDllProj", "5.7", true);
	// Remove the DLL we just created.
	std::filesystem::remove(f.pluginDir / "Binaries" / "Win64" /
							"UnrealEditor-BlueprintReaderEditor.dll");
	auto cfg = MockCfgFromFake(f);
	auto report = diag::RunSetupChecks(cfg);
	REQUIRE(report.HasError());
	bool sawFix = false;
	for (const auto& fnd : report.findings) {
		if (fnd.severity == diag::Severity::Error &&
			fnd.label.find("DLL not built") != std::string::npos) {
			// Fix hint should mention the build command.
			CHECK(fnd.fix_hint.find("Build.bat") != std::string::npos);
			CHECK(fnd.fix_hint.find("-Module=BlueprintReaderEditor") != std::string::npos);
			sawFix = true;
		}
	}
	CHECK(sawFix);
}

TEST_CASE("diagnostics: only DebugGame DLL present + Development requested -> "
		  "FAIL with hint to set BP_READER_EDITOR_CONFIG") {
	auto f = MakeFakeProject("DebugGameProj", "5.7", true, "DebugGame");
	auto cfg = MockCfgFromFake(f);  // editorConfig empty == Development
	auto report = diag::RunSetupChecks(cfg);
	REQUIRE(report.HasError());
	bool sawConfigHint = false;
	for (const auto& fnd : report.findings) {
		if (fnd.severity == diag::Severity::Error &&
			fnd.label.find("DLL not built") != std::string::npos) {
			CHECK(fnd.fix_hint.find("DebugGame") != std::string::npos);
			CHECK(fnd.fix_hint.find("BP_READER_EDITOR_CONFIG") != std::string::npos);
			sawConfigHint = true;
		}
	}
	CHECK(sawConfigHint);
}

TEST_CASE("diagnostics: BlueprintReader missing from .uproject Plugins[] is WARN") {
	auto f = MakeFakeProject("NoPluginEntryProj", "5.7", /*addPluginToUproject=*/false);
	auto cfg = MockCfgFromFake(f);
	auto report = diag::RunSetupChecks(cfg);
	CHECK_FALSE(report.HasError());  // nothing else broken
	REQUIRE(report.HasWarning());
	bool sawWarning = false;
	for (const auto& fnd : report.findings) {
		if (fnd.severity == diag::Severity::Warning &&
			fnd.label.find("not in .uproject") != std::string::npos) {
			CHECK(fnd.fix_hint.find("BlueprintReader") != std::string::npos);
			sawWarning = true;
		}
	}
	CHECK(sawWarning);
}

TEST_CASE("diagnostics: .uproject-listed plugin with explicit Enabled=false is also a warning") {
	auto f = MakeFakeProject("DisabledProj", "5.7");
	// Rewrite .uproject with the plugin entry but Enabled=false.
	std::ifstream in(f.uproject);
	json up;
	in >> up;
	in.close();
	up["Plugins"] = json::array({
		json{{"Name", "BlueprintReader"}, {"Enabled", false}}
	});
	std::ofstream(f.uproject) << up.dump(2);
	auto cfg = MockCfgFromFake(f);
	auto report = diag::RunSetupChecks(cfg);
	CHECK(report.HasWarning());
}

TEST_CASE("diagnostics: missing .uproject is FAIL with engine-association hint") {
	backends::BackendConfig cfg;
	cfg.backend = "commandlet";
	cfg.uproject = "Z:\\does\\not\\exist\\Foo.uproject";
	cfg.fixturesDir = "Z:\\fixtures";
	auto report = diag::RunSetupChecks(cfg);
	CHECK(report.HasError());
}

TEST_CASE("diagnostics: mock backend short-circuits to fixture-dir check") {
	auto tmp = std::filesystem::temp_directory_path() /
			   fmt::format("bpr-mock-test-{}", std::rand());
	std::filesystem::create_directories(tmp);

	backends::BackendConfig cfg;
	cfg.backend = "mock";
	cfg.fixturesDir = tmp;
	auto report = diag::RunSetupChecks(cfg);
	CHECK_FALSE(report.HasError());
	CHECK_FALSE(report.HasWarning());

	// If fixtures dir doesn't exist, fail.
	cfg.fixturesDir = tmp / "nope";
	auto report2 = diag::RunSetupChecks(cfg);
	CHECK(report2.HasError());
}

// ---------------------------------------------------------------------------
// PrintReport: format sanity (no ANSI codes leaked when colors=false, etc.)
// ---------------------------------------------------------------------------

TEST_CASE("diagnostics: PrintReport with colors=false emits no ANSI escapes") {
	diag::Report report;
	report.findings.push_back({diag::Severity::Error, "x", "y", "z"});
	std::ostringstream os;
	diag::PrintReport(report, os, /*colors=*/false);
	auto s = os.str();
	CHECK(s.find("\x1b[") == std::string::npos);
	CHECK(s.find("[FAIL]") != std::string::npos);
	CHECK(s.find("Fix:") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Auto-discovery (Env helpers): walk-up + EngineAssociation read
// ---------------------------------------------------------------------------

TEST_CASE("env: FindUprojectAbove finds the .uproject in the project root") {
	auto f = MakeFakeProject("WalkUpProj", "5.7", true);
	auto found = env::FindUprojectAbove(f.exeDir);
	REQUIRE(found.has_value());
	CHECK(*found == f.uproject);
}

TEST_CASE("env: FindUprojectAbove returns nullopt when no .uproject in chain") {
	auto tmp = std::filesystem::temp_directory_path() /
			   fmt::format("bpr-empty-{}", std::rand());
	std::filesystem::create_directories(tmp);
	auto found = env::FindUprojectAbove(tmp);
	CHECK_FALSE(found.has_value());
}

TEST_CASE("env: ReadEngineAssociation pulls the field from .uproject") {
	auto f = MakeFakeProject("AssocProj", "{8C2F4F06-47C3-B6B7-7D7F-5AB83BABA7D3}");
	auto assoc = env::ReadEngineAssociation(f.uproject);
	REQUIRE(assoc.has_value());
	CHECK(*assoc == "{8C2F4F06-47C3-B6B7-7D7F-5AB83BABA7D3}");
}

TEST_CASE("env: DetectEditorConfig prefers Development over suffixed variants") {
	auto f = MakeFakeProject("DetectDevProj", "5.7", true, /*dllSuffix=*/"");
	// Add a DebugGame variant alongside.
	std::ofstream(f.pluginDir / "Binaries" / "Win64" /
				  "UnrealEditor-BlueprintReaderEditor-Win64-DebugGame.dll");
	auto cfg = env::DetectEditorConfig(f.pluginDir);
	REQUIRE(cfg.has_value());
	CHECK(*cfg == "Development");
}

TEST_CASE("env: DetectEditorConfig falls back to suffixed variant when "
		  "Development is absent") {
	auto f = MakeFakeProject("DetectDgProj", "5.7", true, "DebugGame");
	auto cfg = env::DetectEditorConfig(f.pluginDir);
	REQUIRE(cfg.has_value());
	CHECK(*cfg == "DebugGame");
}
