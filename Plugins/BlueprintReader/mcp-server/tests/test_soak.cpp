// Soak / large-project simulation tests. These run thousands of MCP tool
// calls against the mock backend and assert nothing degrades — response
// shape stays correct, no crash, no output corruption.
//
// Tagged into a "soak" test-suite so they don't run by default. Opt in with:
//
//     bp-reader-tests --test-suite=soak
//
// Default suite stays under a second; soak takes a few seconds to a minute
// depending on the iteration count.
//
// Why test-suite gate: normal CI / local builds shouldn't slow down by 30 s
// on every run. But we want the option to throw 10k requests at the server
// when shaking out a transport-layer change.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace bpr;
using namespace bpr::jsonrpc;
using nlohmann::json;

namespace {

struct DrainResult {
    std::vector<json> frames;
    std::string log;
};

DrainResult RunServerOn(const std::string& input) {
    backends::MockBlueprintReader reader(test::FixturesDir());
    tools::ToolRegistry registry;
    tools::RegisterBlueprintTools(registry, reader);
    Server server;
    mcp::ServerInfo info;
    mcp::RegisterHandlers(server, registry, info);

    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream log;
    server.Run(in, out, log);

    DrainResult r;
    r.log = log.str();
    auto raw = out.str();

    // Detect once and parse all (mock backend mirrors format consistently
    // for the whole session).
    std::size_t i = 0;
    while (i < raw.size() && (raw[i] == '\r' || raw[i] == '\n' ||
                              raw[i] == ' ' || raw[i] == '\t')) ++i;
    if (i >= raw.size()) return r;

    if (raw[i] == '{' || raw[i] == '[') {
        std::string buf;
        for (; i < raw.size(); ++i) {
            if (raw[i] == '\n') {
                if (!buf.empty() && buf.back() == '\r') buf.pop_back();
                if (!buf.empty()) r.frames.push_back(json::parse(buf));
                buf.clear();
            } else {
                buf.push_back(raw[i]);
            }
        }
        if (!buf.empty()) r.frames.push_back(json::parse(buf));
    } else {
        while (i < raw.size()) {
            auto split = raw.find("\r\n\r\n", i);
            if (split == std::string::npos) break;
            auto headers = raw.substr(i, split - i);
            auto pos = headers.find("Content-Length:");
            REQUIRE(pos != std::string::npos);
            std::size_t end = headers.find_first_of("\r\n", pos);
            std::size_t len = std::stoul(
                headers.substr(pos + 15, end - pos - 15));
            std::size_t bodyStart = split + 4;
            r.frames.push_back(json::parse(raw.substr(bodyStart, len)));
            i = bodyStart + len;
        }
    }
    return r;
}

std::string FrameNL(const json& body) {
    std::ostringstream os;
    WriteFrame(os, body, FrameFormat::NewlineDelimited);
    return os.str();
}

json InitReq(int id = 0) {
    return json{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
        {"params", json{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"clientInfo", json{{"name", "soak"}, {"version", "0"}}}}}};
}

} // namespace

TEST_CASE("soak: 5000 mixed read tool calls, all return correct shape"
          * doctest::test_suite("soak")) {
    // Fixtures-backed: list_blueprints, read_blueprint, get_graph, etc. all
    // hit the mock. We're stressing the dispatch + tool-registry +
    // serialization path. Random tool selection avoids bias from a single
    // hot path masking issues on a colder one.
    constexpr int kRequestCount = 5000;

    std::vector<std::string> readTools = {
        "list_blueprints", "read_blueprint", "get_graph", "get_function",
        "list_variables", "get_components", "find_node",
        "list_node_kinds", "list_pin_categories"
    };

    auto argsFor = [](const std::string& tool) -> json {
        if (tool == "list_blueprints")     return json{{"path", "/Game"}};
        if (tool == "read_blueprint")      return json{{"asset_path", "/Game/AI/BP_Enemy"}};
        if (tool == "get_graph")           return json{{"asset_path", "/Game/AI/BP_Enemy"}};
        if (tool == "get_function")        return json{{"asset_path", "/Game/AI/BP_Enemy"},
                                                       {"function_name", "TakeDamage"}};
        if (tool == "list_variables")      return json{{"asset_path", "/Game/AI/BP_Enemy"}};
        if (tool == "get_components")      return json{{"asset_path", "/Game/AI/BP_Enemy"}};
        if (tool == "find_node")           return json{{"asset_path", "/Game/AI/BP_Enemy"},
                                                       {"query", "Damage"}};
        return json::object(); // list_node_kinds, list_pin_categories
    };

    std::mt19937 rng(0xBEEF);
    std::uniform_int_distribution<std::size_t> pick(0, readTools.size() - 1);

    std::string in = FrameNL(InitReq(0));
    for (int i = 1; i <= kRequestCount; ++i) {
        const auto& tool = readTools[pick(rng)];
        in += FrameNL(json{
            {"jsonrpc", "2.0"}, {"id", i}, {"method", "tools/call"},
            {"params", json{{"name", tool}, {"arguments", argsFor(tool)}}}});
    }

    auto t0 = std::chrono::steady_clock::now();
    auto r = RunServerOn(in);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    MESSAGE("soak: dispatched " << kRequestCount << " tool calls in " << dt << " ms"
            << " (" << (double)dt / kRequestCount << " ms/call avg)");

    REQUIRE(r.frames.size() == static_cast<std::size_t>(kRequestCount + 1));

    // Every response has the right id and either a result or a real error
    // (errors get an "error" envelope; we don't expect any here from the
    // mock backend with read-only tools).
    int errorCount = 0;
    for (int i = 0; i <= kRequestCount; ++i) {
        const auto& f = r.frames[i];
        CHECK(f["id"] == i);
        CHECK(f["jsonrpc"] == "2.0");
        if (f.contains("error")) ++errorCount;
    }
    CHECK(errorCount == 0);
}

TEST_CASE("soak: 1000 calls each through mixed tool surface, both framings"
          * doctest::test_suite("soak")) {
    // Stresses the format-mirroring path: alternate sessions in each format,
    // verify both produce correct output.
    constexpr int kPerFormat = 1000;

    auto buildSession = [&](FrameFormat fmt) -> std::string {
        std::ostringstream os;
        WriteFrame(os, InitReq(0), fmt);
        for (int i = 1; i <= kPerFormat; ++i) {
            WriteFrame(os, json{
                {"jsonrpc", "2.0"}, {"id", i}, {"method", "tools/call"},
                {"params", json{{"name", "list_pin_categories"},
                                {"arguments", json::object()}}}}, fmt);
        }
        return os.str();
    };

    SUBCASE("newline-delimited") {
        auto r = RunServerOn(buildSession(FrameFormat::NewlineDelimited));
        REQUIRE(r.frames.size() == kPerFormat + 1);
        for (int i = 0; i <= kPerFormat; ++i) {
            CHECK(r.frames[i]["id"] == i);
            CHECK(!r.frames[i].contains("error"));
        }
    }
    SUBCASE("content-length") {
        auto r = RunServerOn(buildSession(FrameFormat::ContentLength));
        REQUIRE(r.frames.size() == kPerFormat + 1);
        for (int i = 0; i <= kPerFormat; ++i) {
            CHECK(r.frames[i]["id"] == i);
            CHECK(!r.frames[i].contains("error"));
        }
    }
}

TEST_CASE("soak: large-response handling - find_node with broad query"
          * doctest::test_suite("soak")) {
    // Hammering find_node 500x with permissive queries against fixtures
    // generates a steady stream of larger response payloads. Any
    // accumulator or buffer-reuse bug in the framing layer would show up
    // as truncated / interleaved output.
    constexpr int kIters = 500;

    std::ostringstream os;
    WriteFrame(os, InitReq(0), FrameFormat::NewlineDelimited);
    for (int i = 1; i <= kIters; ++i) {
        WriteFrame(os, json{
            {"jsonrpc", "2.0"}, {"id", i}, {"method", "tools/call"},
            {"params", json{{"name", "find_node"},
                            {"arguments", json{{"asset_path", "/Game/AI/BP_Enemy"},
                                               {"query", ""}}}}}},
            FrameFormat::NewlineDelimited);
    }
    auto r = RunServerOn(os.str());
    REQUIRE(r.frames.size() == kIters + 1);
    for (int i = 1; i <= kIters; ++i) {
        // Each response is a tools/call envelope with content + isError.
        CHECK(r.frames[i]["id"] == i);
        REQUIRE(r.frames[i].contains("result"));
        // The mock returns a content array with at least the JSON tool
        // output; a missing or malformed payload would mean serialization
        // dropped data on the floor.
        CHECK(r.frames[i]["result"]["content"].is_array());
        CHECK(r.frames[i]["result"]["content"].size() >= 1);
    }
}

TEST_CASE("soak: simulated 'big project' - many distinct asset paths in sequence"
          * doctest::test_suite("soak")) {
    // The mock has three fixture BPs but we hammer list_blueprints with a
    // bunch of different content roots to simulate a large project's path
    // diversity. Any caching / map-reuse bug surfaces here.
    std::vector<std::string> roots = {
        "/Game", "/Game/AI", "/Game/AI/Bosses", "/Game/AI/Mooks",
        "/Game/Player", "/Game/UI", "/Game/Effects/Particles",
        "/Game/Maps/Cave/Decals", "/Game/Items/Weapons/Heavy",
        "/Engine/Functions", "/Engine/MannequinSkeleton"
    };
    constexpr int kRounds = 500;

    std::ostringstream os;
    WriteFrame(os, InitReq(0), FrameFormat::NewlineDelimited);
    int idCounter = 1;
    for (int round = 0; round < kRounds; ++round) {
        for (const auto& root : roots) {
            WriteFrame(os, json{
                {"jsonrpc", "2.0"}, {"id", idCounter++}, {"method", "tools/call"},
                {"params", json{{"name", "list_blueprints"},
                                {"arguments", json{{"path", root}}}}}},
                FrameFormat::NewlineDelimited);
        }
    }
    auto expected = static_cast<std::size_t>(idCounter);  // includes initialize id 0
    auto r = RunServerOn(os.str());
    REQUIRE(r.frames.size() == expected);
    for (std::size_t i = 0; i < expected; ++i) {
        CHECK(r.frames[i]["id"] == static_cast<int>(i));
    }
}
