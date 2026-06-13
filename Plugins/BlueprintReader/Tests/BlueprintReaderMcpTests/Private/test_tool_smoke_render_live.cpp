// Live render smoke (TEST-1): drive the render/interactive tool surface against
// a REAL rendering editor (Track B: UnrealEditor.exe -RenderOffscreen, a true
// D3D12 RHI) through the full tool-handler -> live-socket stack. These are the
// ~tools the -nullrhi daemon could only registration-check (camera, show flags,
// view mode, viewport projection, selection, screenshot): on a rendering editor
// they actually execute. Asserts reachability (no 'not supported' / unreachable
// / crash) PLUS a few render-tier-proving checks (a screenshot really captures;
// world->screen projection is valid).
//
// Gated + opt-in: runs only when BP_READER_SMOKE_RENDER is set AND a render-
// editor handshake (<Project>/Saved/bp-reader-live.json, from BP_READER_PROJECT)
// is present. The render editor must be GPU/-RenderOffscreen (CanEverRender);
// a headless -nullrhi one returns captured:false (honest) and would fail the
// screenshot check — which is the point: this smoke certifies the render tier.
// The hosted CI has no editor, so it auto-skips.

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

std::string RenderSmokeEnv(const char* key) {
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

std::filesystem::path RenderHandshakePath() {
	const std::string proj = RenderSmokeEnv("BP_READER_PROJECT");
	if (proj.empty()) {
		return {};
	}
	return std::filesystem::path(proj).parent_path() / "Saved" / "bp-reader-live.json";
}

bool RenderSmokeEnabled() {
	if (RenderSmokeEnv("BP_READER_SMOKE_RENDER").empty()) {
		return false;
	}
	const auto hs = RenderHandshakePath();
	std::error_code ec;
	return !hs.empty() && std::filesystem::exists(hs, ec);
}

bool RenderLooksBroken(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("not supported by this backend") || has("Unsupported op") ||
	       has("unknown tool") || has("no such tool") || has("not implemented");
}

bool RenderLooksInfra(const std::string& msg) {
	auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
	return has("transport") || has("connection closed") || has("handshake") ||
	       has("timed out") || has("timeout") || has("never appeared");
}

}    // namespace

TEST_CASE("[live][smoke][render] render/interactive tool surface drives a real rendering editor"
          * doctest::skip(!RenderSmokeEnabled())) {
	const auto hsPath = RenderHandshakePath();
	std::ifstream hsFile(hsPath);
	REQUIRE_MESSAGE(hsFile.is_open(), "render-editor handshake unreadable: " << hsPath.string());
	json hs;
	hsFile >> hs;

	bpr::backends::SocketBlueprintReader::Config cfg;
	cfg.host  = hs.value("host", std::string("127.0.0.1"));
	cfg.port  = hs.value("port", 0);
	cfg.token = hs.value("token", std::string());
	cfg.handshakeFilePath = hsPath.string();
	REQUIRE(cfg.port != 0);
	REQUIRE_FALSE(cfg.token.empty());
	bpr::backends::SocketBlueprintReader reader(cfg);

	bpr::tools::ToolRegistry registry;
	bpr::tools::RegisterBlueprintTools(registry, reader);

	std::vector<std::string> broken;
	std::vector<std::string> infra;
	int dispatched = 0, okOrAcceptable = 0;

	auto drive = [&](const std::string& name, const json& args) -> json {
		const auto* fn = registry.Find(name);
		REQUIRE_MESSAGE(fn != nullptr, "render tool not registered: " << name);
		++dispatched;
		try {
			json r = (*fn)(args);
			++okOrAcceptable;
			return r;
		} catch (const std::exception& e) {
			const std::string msg = e.what();
			if (RenderLooksBroken(msg)) {
				broken.push_back(name + " -> " + msg);
			} else if (RenderLooksInfra(msg)) {
				infra.push_back(name + " -> " + msg);
			} else {
				++okOrAcceptable;
			}
			return json(nullptr);
		}
	};

	// ---- viewport / camera / show-flag / view-mode READS ----
	const json cam = drive("get_camera_transform", json::object());
	drive("get_show_flags", json::object());
	drive("get_view_mode", json::object());
	drive("get_selected_actors", json::object());

	// ---- safe view-state WRITES (reversible, no asset mutation) ----
	drive("set_camera_transform", json{{"loc_x", 0.0}, {"loc_y", 0.0}, {"loc_z", 500.0},
	                                   {"rot_pitch", -30.0}, {"rot_yaw", 0.0}, {"rot_roll", 0.0}});
	drive("set_show_flag", json{{"flag_name", "Wireframe"}, {"enabled", true}});
	drive("set_show_flag", json{{"flag_name", "Wireframe"}, {"enabled", false}});    // restore
	drive("set_view_mode", json{{"mode", "Lit"}});
	drive("set_selection", json{{"actor_names", json::array()}, {"replace", true}});    // clear

	// ---- viewport projection (round-trip-ish) ----
	const json w2s = drive("world_pos_to_screen", json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}});
	drive("screen_to_world", json{{"screen_x", 0.5}, {"screen_y", 0.5}, {"max_distance", 100000.0}});

	// ---- a real capture: certifies the render tier actually renders ----
	const auto shotPath = (std::filesystem::temp_directory_path() / "bpr_smoke_render.png").string();
	const json shot = drive("take_screenshot", json{{"dest_path", shotPath}, {"width", 320}, {"height", 180}});

	// ---- render-tier-proving assertions (only when the tool ran) ----
	if (shot.is_object()) {
		// A GPU/-RenderOffscreen editor captures; a headless one returns
		// captured:false. This is THE proof the render tier is live.
		CHECK_MESSAGE(shot.value("captured", false) == true,
		              "take_screenshot captured:false — render tier not active? note: "
		              << shot.value("note", std::string()));
	}
	if (w2s.is_object()) {
		// A live viewport can project; valid:false means no viewport.
		CHECK(w2s.value("valid", false) == true);
	}
	if (cam.is_object()) {
		// Camera transform read returns a real viewport pose (shape:
		// {valid, loc_x/y/z, pitch/yaw/roll, fov} — `valid`, not `ok`).
		CHECK(cam.value("valid", false) == true);
	}

	MESSAGE("render smoke: " << dispatched << " dispatched, " << okOrAcceptable
	        << " ok/acceptable, " << infra.size() << " infra, " << broken.size()
	        << " broken");
	for (const auto& b : broken) { CAPTURE(b); }
	for (const auto& i : infra) { CAPTURE(i); }
	CHECK(broken.empty());
	CHECK(infra.empty());
}
