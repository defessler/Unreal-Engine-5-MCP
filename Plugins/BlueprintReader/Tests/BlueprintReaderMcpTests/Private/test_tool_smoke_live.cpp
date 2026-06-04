// Live smoke: dispatch EVERY registered MCP tool against a REAL editor
// (commandlet backend) and assert none is unreachable, "not supported by
// this backend", or crashing. This proves the full 252-tool surface is
// wired end to end through the backend decorator chain to a running
// editor — the failure mode the mock suite (which implements everything)
// cannot catch.
//
// This is a REACHABILITY / INTEGRATION smoke, not a per-tool functional
// test. Most asset-scoped tools are called against a deliberately
// NON-EXISTENT asset path, so they dispatch and hit a benign "asset not
// found" with zero side effects (no mutation, no deletion). No-asset
// ACTION/lifecycle tools (PIE, console, python, build, daemon shutdown,
// project settings, viewport/selection mutators) can't be made benign by
// a fake asset path, so they are registration-checked only. Functional
// correctness of the core tools is covered by the mock suite plus the
// live [roundtrip][granular] test (BP -> Spec -> BP, zero drift).
//
// Gated + opt-in: runs only when BP_READER_ENGINE_DIR + BP_READER_PROJECT
// are set AND BP_READER_SMOKE_ALL is set, because it drives a real editor
// daemon and issues ~220 calls.

#include <doctest/doctest.h>

#include "tools/ToolRegistry.h"
#include "tools/BlueprintTools.h"
#include "backends/CommandletBlueprintReader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using nlohmann::json;

std::string SmokeEnv(const char* key) {
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

bool SmokeEnabled() {
	return !SmokeEnv("BP_READER_ENGINE_DIR").empty() &&
	       !SmokeEnv("BP_READER_PROJECT").empty() &&
	       !SmokeEnv("BP_READER_SMOKE_ALL").empty();
}

// Non-asset-scoped tools that would perturb or kill the editor if called
// (the "non-existent asset" safety net doesn't apply — they take no
// asset_path). Registration-checked only.
const std::set<std::string, std::less<>> kActionDenylist = {
	"shutdown_daemon", "restart_editor", "quit_editor", "quit", "exit",
	"pie_start", "pie_stop", "start_pie", "stop_pie", "save_all",
	"build_lighting", "run_automation_tests", "cook_content", "package_project",
	"console_command", "run_console_command", "run_python_script",
	"set_cvar", "set_project_setting", "reset_project_setting",
	"live_coding_compile", "spawn_actor", "delete_actor", "set_actor_transform",
	"focus_actor", "set_camera_transform", "set_selection", "set_show_flag",
	"set_selected_assets", "take_screenshot", "take_viewport_screenshot",
	"take_annotated_screenshot", "open_asset_editor", "start_profile",
	"stop_profile",
};

// Best-effort placeholders for common required fields. A deliberately
// non-existent asset path makes every asset-scoped tool (read OR write OR
// delete) dispatch and return a benign not-found with zero side effects.
json SmokeArgs(const json& inputSchema) {
	static const char* kMissingAsset = "/Game/__bpr_smoke_missing__";
	json args = json::object();
	if (!inputSchema.is_object() || !inputSchema.contains("required")) {
		return args;
	}
	for (const auto& reqField : inputSchema["required"]) {
		const std::string f = reqField.get<std::string>();
		if (f == "asset_path" || f == "asset_path1" || f == "asset_path2" ||
		    f == "source_asset" || f == "dest_path" || f == "new_path" ||
		    f == "mesh_path" || f == "instance_path" || f == "path") {
			args[f] = kMissingAsset;
		} else if (f == "graph_name") {
			args[f] = "EventGraph";
		} else if (f == "function_name") {
			args[f] = "__smoke__";
		} else if (f == "node_id" || f == "from_node" || f == "to_node") {
			args[f] = "00000000000000000000000000000000";
		} else if (f == "name" || f == "new_name" || f == "old_name") {
			args[f] = "__smoke__";
		} else if (f == "kind") {
			args[f] = "Branch";
		} else if (f == "class_path") {
			args[f] = "/Script/Engine.Actor";
		} else if (f == "class_name") {
			args[f] = "Actor";
		} else if (f == "cvar_name") {
			args[f] = "r.__bpr_smoke__";
		} else if (f == "type") {
			args[f] = "bool";
		} else if (f == "value") {
			args[f] = "0";
		} else if (f == "ops") {
			args[f] = json::array();
		} else if (f == "cpp_source") {
			args[f] = "void F(){}";
		} else {
			args[f] = "__smoke__";  // generic placeholder; tool may reject (acceptable)
		}
	}
	return args;
}

bool LooksBroken(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("not supported by this backend") || has("Unsupported op") ||
	       has("unknown tool") || has("no such tool") ||
	       has("not implemented");
}

bool LooksInfra(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("transport") || has("connection closed") || has("handshake") ||
	       has("timed out") || has("timeout") || has("never appeared");
}

}    // namespace

TEST_CASE("[live][smoke] every tool dispatches against a real editor"
          " (no 'not supported' / unreachable / crash)"
          * doctest::skip(!SmokeEnabled())) {
	bpr::backends::CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(SmokeEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(SmokeEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(90);
	cfg.useDaemon = true;    // one editor daemon, amortized across all calls
	bpr::backends::CommandletBlueprintReader reader(std::move(cfg));

	bpr::tools::ToolRegistry registry;
	bpr::tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	REQUIRE(spec.size() == 258);  // bump alongside the other tool-count asserts

	std::vector<std::string> broken;    // unreachable / not-supported / crash
	std::vector<std::string> infra;     // transport/daemon issues (environmental)
	int dispatched = 0, denylisted = 0, okOrAcceptable = 0;

	for (const auto& t : spec) {
		const std::string name = t["name"].get<std::string>();
		const auto* fn = registry.Find(name);
		REQUIRE(fn != nullptr);    // every listed tool must be findable
		if (kActionDenylist.count(name) != 0) {
			++denylisted;
			continue;
		}
		const json args = SmokeArgs(t.value("inputSchema", json::object()));
		++dispatched;
		try {
			(void)(*fn)(args);
			++okOrAcceptable;
		} catch (const std::exception& e) {
			const std::string msg = e.what();
			if (LooksBroken(msg)) {
				broken.push_back(name + " -> " + msg);
			} else if (LooksInfra(msg)) {
				infra.push_back(name + " -> " + msg);
			} else {
				// validation / not-found / precondition / disabled =
				// the tool is wired and ran its handler against the editor.
				++okOrAcceptable;
			}
		}
	}

	MESSAGE("smoke: " << dispatched << " dispatched, " << denylisted
	        << " action-denylisted (registration-checked), " << okOrAcceptable
	        << " ok/acceptable, " << infra.size() << " infra, " << broken.size()
	        << " broken");
	for (const auto& b : broken) {
		CAPTURE(b);
	}
	for (const auto& i : infra) {
		CAPTURE(i);
	}
	CHECK(broken.empty());
	// Infra errors aren't tool bugs, but mean the smoke couldn't reach the
	// editor for some tools — surface loudly so results aren't trusted blind.
	CHECK(infra.empty());
}
