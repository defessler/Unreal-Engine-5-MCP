// Live UI smoke (TEST-2 P2): drive the editor-UI tool surface against a REAL
// render editor (Track B) through the full tool-handler -> live-socket stack,
// and assert every UI tool is reachable + returns a sane shape — the UI analog
// of test_tool_smoke_live's BP_READER_SMOKE_ALL. A reachability / integration
// smoke (no 'not supported by this backend', no crash), NOT a per-tool
// functional test. Per-tool functional correctness is covered by the dedicated
// Saved/verify-ui-*.ps1 drills + the modal-recovery drill (verify-test2-p1a.ps1).
//
// Gated + opt-in: runs only when BP_READER_SMOKE_UI is set AND a render-editor
// handshake (<Project>/Saved/bp-reader-live.json, from BP_READER_PROJECT) is
// present. The render editor must have been started with BP_READER_ALLOW_UI=1
// (Saved/start-render-editor.ps1 + the env) so the GATED ui_focus_tab /
// ui_invoke_menu actually execute rather than returning their gate error — a
// gate rejection is still treated as "reachable" (wired, just gated), never
// "broken". The hosted CI has no editor, so this auto-skips there.

#include <doctest/doctest.h>

#include "backends/SocketBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using nlohmann::json;

std::string UiSmokeEnv(const char* key) {
#ifdef _MSC_VER
	char* buf = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
		std::string out(buf);
		std::free(buf);
		return out;
	}
	return {};
#else
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
		return std::string(v);
	}
	return {};
#endif
}

std::filesystem::path UiHandshakePath() {
	const std::string proj = UiSmokeEnv("BP_READER_PROJECT");
	if (proj.empty()) {
		return {};
	}
	// <Project>/Saved/bp-reader-live.json — the render editor's published port+token.
	return std::filesystem::path(proj).parent_path() / "Saved" / "bp-reader-live.json";
}

bool UiSmokeEnabled() {
	if (UiSmokeEnv("BP_READER_SMOKE_UI").empty()) {
		return false;
	}
	const auto hs = UiHandshakePath();
	std::error_code ec;
	return !hs.empty() && std::filesystem::exists(hs, ec);
}

// A response is "broken" only if the tool is unreachable / not wired — never
// for a benign functional outcome (a gate rejection, a not-found, etc.).
bool UiLooksBroken(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("not supported by this backend") || has("Unsupported op") ||
	       has("unknown tool") || has("no such tool") || has("not implemented");
}

bool UiLooksInfra(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("transport") || has("connection closed") || has("handshake") ||
	       has("timed out") || has("timeout") || has("never appeared");
}

}    // namespace

TEST_CASE("[live][smoke][ui] editor-UI tool surface drives a real render editor"
          " (no 'not supported' / unreachable / crash)"
          * doctest::skip(!UiSmokeEnabled())) {
	// Read the render editor's handshake for port + token.
	const auto hsPath = UiHandshakePath();
	std::ifstream hsFile(hsPath);
	REQUIRE_MESSAGE(hsFile.is_open(), "render-editor handshake unreadable: " << hsPath.string());
	json hs;
	hsFile >> hs;

	bpr::backends::SocketBlueprintReader::Config cfg;
	cfg.host  = hs.value("host", std::string("127.0.0.1"));
	cfg.port  = hs.value("port", 0);
	cfg.token = hs.value("token", std::string());
	cfg.handshakeFilePath = hsPath.string();    // re-probe on a port/token rotation
	REQUIRE(cfg.port != 0);
	REQUIRE_FALSE(cfg.token.empty());
	bpr::backends::SocketBlueprintReader reader(cfg);

	bpr::tools::ToolRegistry registry;
	bpr::tools::RegisterBlueprintTools(registry, reader);

	std::vector<std::string> broken;    // unreachable / not-wired / crash
	std::vector<std::string> infra;     // transport/editor-gone (environmental)
	int dispatched = 0, okOrAcceptable = 0;

	auto drive = [&](const std::string& name, const json& args) -> json {
		const auto* fn = registry.Find(name);
		REQUIRE_MESSAGE(fn != nullptr, "UI tool not registered: " << name);
		++dispatched;
		try {
			json r = (*fn)(args);
			++okOrAcceptable;
			return r;
		} catch (const std::exception& e) {
			const std::string msg = e.what();
			if (UiLooksBroken(msg)) {
				broken.push_back(name + " -> " + msg);
			} else if (UiLooksInfra(msg)) {
				infra.push_back(name + " -> " + msg);
			} else {
				++okOrAcceptable;    // functional outcome — the tool ran end to end
			}
			return json(nullptr);
		}
	};

	// ---- read-only UI inspectors (ungated) ----
	const json widgets = drive("ui_list_widgets", json::object());
	if (widgets.is_object()) {
		// On a real render editor the Slate tree is live: ui_available + windows.
		CHECK(widgets.value("ui_available", false) == true);
		CHECK(widgets.contains("windows"));
	}
	drive("get_modal_state", json::object());
	drive("get_focused_widget", json::object());

	// ---- geometry-independent drivers (gated BP_READER_ALLOW_UI on the editor) ----
	// Action path + the structured NotFound path. ui_focus_tab / ui_invoke_menu
	// THROW a structured NotFound on a bogus target (caught as acceptable), so
	// to actually exercise the ACTION path we target common, always-present
	// level-editor tabs / a safe reversible command directly rather than deriving
	// from the (throwing) discovery call.
	drive("ui_focus_tab", json{{"tab_label", "Outliner"}});    // foreground a real dock tab
	drive("ui_focus_tab", json{{"tab_label", "Details"}});
	drive("ui_focus_tab", json{{"tab_label", "___bpr_smoke_no_tab___"}});    // NotFound path

	// ui_invoke_menu: the NotFound path (lists available_entries) + a real,
	// SAFE, reversible command — SelectNone clears the actor selection; no
	// asset mutation, idempotent.
	drive("ui_invoke_menu", json{{"menu", "LevelEditor.MainMenu.Select"},
	                             {"entry", "___bpr_smoke_no_entry___"}});
	drive("ui_invoke_menu", json{{"menu", "LevelEditor.MainMenu.Select"},
	                             {"entry", "SelectNone"}});

	MESSAGE("ui smoke: " << dispatched << " dispatched, " << okOrAcceptable
	        << " ok/acceptable, " << infra.size() << " infra, " << broken.size()
	        << " broken");
	for (const auto& b : broken) { CAPTURE(b); }
	for (const auto& i : infra) { CAPTURE(i); }
	CHECK(broken.empty());
	CHECK(infra.empty());
}
