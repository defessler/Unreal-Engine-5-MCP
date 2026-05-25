// Phase 10 (EA-push) tests: EditorSubscriptions registry + the
// editor/subscribe / editor/unsubscribe JSON-RPC methods + the
// experimental.editor capability advertisement (opt-in via a wired
// EditorSubscriptions; absent yields -32601).

#include <doctest/doctest.h>

#include "jsonrpc/Mcp.h"
#include "jsonrpc/Server.h"
#include "tools/BlueprintTools.h"
#include "tools/EditorSubscriptions.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <string>

using namespace bpr;
using nlohmann::json;

// =====================================================================
// EditorSubscriptions registry
// =====================================================================

TEST_CASE("EditorSubscriptions: subscribe / unsubscribe / IsSubscribed") {
	tools::EditorSubscriptions s;
	CHECK(s.Count() == 0);

	const std::string id = s.Subscribe({"asset_opened", "asset_closed"});
	CHECK_FALSE(id.empty());
	CHECK(s.Count() == 1);
	CHECK(s.IsSubscribed("asset_opened"));
	CHECK(s.IsSubscribed("asset_closed"));
	CHECK_FALSE(s.IsSubscribed("pie_started"));

	// Empty subscribe == "all events".
	const std::string idAll = s.Subscribe({});
	CHECK(s.Count() == 2);
	CHECK(s.IsSubscribed("pie_started"));    // now covered by the all-subscription

	CHECK(s.Unsubscribe(id));
	CHECK(s.Count() == 1);
	CHECK_FALSE(s.Unsubscribe("does-not-exist"));

	CHECK(s.Unsubscribe(idAll));
	CHECK(s.Count() == 0);
	CHECK_FALSE(s.IsSubscribed("asset_opened"));
}

// =====================================================================
// editor/subscribe + editor/unsubscribe via the Server
// =====================================================================

namespace {
json Dispatch(jsonrpc::Server& server, const std::string& method, const json& params, int id) {
	auto resp = server.Dispatch(json{
		{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
	REQUIRE(resp.has_value());
	return *resp;
}
}    // namespace

TEST_CASE("editor push: capability advertised + subscribe/unsubscribe when wired") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	tools::EditorSubscriptions subs;
	jsonrpc::Server server;
	mcp::ServerInfo info;
	mcp::RegisterHandlers(server, registry, nullptr, nullptr, nullptr, info, &subs);

	// initialize advertises experimental.editor.
	const json init = Dispatch(server, "initialize", json::object(), 1);
	REQUIRE(init["result"]["capabilities"].contains("experimental"));
	CHECK(init["result"]["capabilities"]["experimental"]["editor"]["events"] == true);

	// editor/subscribe returns an id and records the subscription.
	const json sub = Dispatch(server, "editor/subscribe",
		json{{"event_types", json::array({"level_actor_selection_changed"})}}, 2);
	const std::string subId = sub["result"]["subscription_id"];
	CHECK_FALSE(subId.empty());
	CHECK(subs.Count() == 1);
	CHECK(subs.IsSubscribed("level_actor_selection_changed"));

	// editor/unsubscribe drops it.
	const json unsub = Dispatch(server, "editor/unsubscribe",
		json{{"subscription_id", subId}}, 3);
	CHECK(unsub["result"]["ok"] == true);
	CHECK(subs.Count() == 0);
}

TEST_CASE("editor push: methods absent + capability suppressed when disabled") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	jsonrpc::Server server;
	mcp::ServerInfo info;
	// No EditorSubscriptions wired (default / BP_READER_PUSH_EVENTS off).
	mcp::RegisterHandlers(server, registry, nullptr, nullptr, nullptr, info);

	const json init = Dispatch(server, "initialize", json::object(), 1);
	CHECK_FALSE(init["result"]["capabilities"].contains("experimental"));

	// editor/subscribe is unregistered -> JSON-RPC Method not found (-32601).
	const json sub = Dispatch(server, "editor/subscribe", json::object(), 2);
	REQUIRE(sub.contains("error"));
	CHECK(sub["error"]["code"] == -32601);
}
