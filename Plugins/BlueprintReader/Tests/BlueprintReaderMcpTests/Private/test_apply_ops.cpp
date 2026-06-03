// Tests for apply_ops + the apply_ops surface that compile_function /
// auto_layout_graph also exercise.
//
// We need writable backend semantics so apply_ops can actually run a
// batch end-to-end. The MockBlueprintReader is read-only, so this file
// supplies a small in-memory mutable backend (FakeWritableReader) that
// just tracks which calls came in and mints fake GUIDs for AddNode.
// Good enough to verify dispatch + slot resolution + idempotency.

#include <doctest/doctest.h>

#include "backends/IBlueprintReader.h"
#include "backends/MockBlueprintReader.h"
#include "tools/ApplyOps.h"
#include "jsonrpc/CallContext.h"
#include "jsonrpc/Server.h"

#include "test_helpers.h"

#include <optional>

#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace bpr;
using namespace bpr::backends;
using nlohmann::json;

namespace test_apply_ops_detail {

// Mutable backend: reads delegate to a real MockBlueprintReader, writes
// just record-and-succeed (or record-and-throw if `failOnWrite`).
class FakeWritableReader : public IBlueprintReader {
public:
	FakeWritableReader()
		: inner_(test::FixturesDir()) {}

	struct Call {
		std::string op;
		std::string asset;
		std::string detail;  // e.g. var name, graph name
	};
	std::vector<Call> calls;
	int nextGuidNum = 1;
	int beginBatchCalls = 0;
	int endBatchCalls = 0;
	bool failOnWrite = false;
	// For idempotency tests: synthesized "extra" variables/functions added
	// by writes appear here. Reads merge them in.
	std::map<std::string, std::vector<BPVariable>> extraVars;
	std::map<std::string, std::vector<BPFunctionSummary>> extraFuncs;

	std::string Mint() {
		char buf[37];
		std::snprintf(buf, sizeof(buf),
			"00000000-0000-0000-0000-%012d", nextGuidNum++);
		return buf;
	}

	void Note(std::string op, std::string asset, std::string detail = {}) {
		calls.push_back({std::move(op), std::move(asset), std::move(detail)});
		if (failOnWrite) {
			throw BlueprintReaderError("simulated write failure");
		}
	}

	// ---- reads (delegate, with merged extras) ----
	std::vector<BPAssetSummary> ListBlueprints(std::string_view p) override {
		return inner_.ListBlueprints(p);
	}
	BPMetadata ReadBlueprint(std::string_view a) override {
		BPMetadata m = inner_.ReadBlueprint(a);
		auto it = extraFuncs.find(std::string(a));
		if (it != extraFuncs.end()) {
			for (const auto& f : it->second)
			{
				m.Functions.push_back(f);
			}
		}
		return m;
	}
	BPGraph                  GetGraph(std::string_view a, std::string_view g) override {
		return inner_.GetGraph(a, g);
	}
	BPFunction               GetFunction(std::string_view a, std::string_view f) override {
		return inner_.GetFunction(a, f);
	}
	std::vector<BPVariable>  ListVariables(std::string_view a) override {
		auto v = inner_.ListVariables(a);
		auto it = extraVars.find(std::string(a));
		if (it != extraVars.end()) {
			for (const auto& x : it->second)
			{
				v.push_back(x);
			}
		}
		return v;
	}
	std::vector<BPComponent> GetComponents(std::string_view a) override {
		return inner_.GetComponents(a);
	}
	std::vector<BPNode>      FindNode(std::string_view a, std::string_view q,
									  std::string_view k) override {
		return inner_.FindNode(a, q, k);
	}

	// ---- writes (recorded, or fail) ----
	void AddVariable(std::string_view a, std::string_view n, const BPPinType&,
					 std::string_view, std::string_view, bool, bool) override {
		Note("AddVariable", std::string(a), std::string(n));
		BPVariable v;
		v.Name = std::string(n);
		extraVars[std::string(a)].push_back(std::move(v));
	}
	void SetNodePosition(std::string_view a, std::string_view, std::string_view,
						 int, int) override {
		Note("SetNodePosition", std::string(a));
	}
	void DeleteNode(std::string_view a, std::string_view, std::string_view) override {
		Note("DeleteNode", std::string(a));
	}
	std::string AddNode(std::string_view a, std::string_view, std::string_view kind,
						int, int,
						const std::map<std::string, std::string, std::less<>>&) override {
		Note("AddNode", std::string(a), std::string(kind));
		return Mint();
	}
	void WirePins(std::string_view a, std::string_view,
				  std::string_view, std::string_view,
				  std::string_view, std::string_view) override {
		Note("WirePins", std::string(a));
	}
	void DeleteVariable(std::string_view a, std::string_view n) override {
		Note("DeleteVariable", std::string(a), std::string(n));
	}
	void RenameVariable(std::string_view a, std::string_view, std::string_view) override {
		Note("RenameVariable", std::string(a));
	}
	AddFunctionResult AddFunction(std::string_view a, std::string_view n) override {
		Note("AddFunction", std::string(a), std::string(n));
		BPFunctionSummary fs;
		fs.Name = std::string(n);
		extraFuncs[std::string(a)].push_back(std::move(fs));
		// Mint a deterministic-ish entry-node GUID so OpAddFunction's slot
		// binding has something to write into the SlotMap.
		AddFunctionResult r;
		r.functionName = std::string(n);
		r.entryNodeId  = Mint();
		return r;
	}
	void AddFunctionInput(std::string_view a, std::string_view, std::string_view,
						  const BPPinType&) override {
		Note("AddFunctionInput", std::string(a));
	}
	void AddFunctionOutput(std::string_view a, std::string_view, std::string_view,
						   const BPPinType&) override {
		Note("AddFunctionOutput", std::string(a));
	}
	void DeleteFunction(std::string_view a, std::string_view n) override {
		Note("DeleteFunction", std::string(a), std::string(n));
	}
	void SetVariableDefault(std::string_view a, std::string_view n, std::string_view) override {
		Note("SetVariableDefault", std::string(a), std::string(n));
	}
	CreateBlueprintResult CreateBlueprint(std::string_view a, std::string_view p,
										  std::string_view bt) override {
		Note("CreateBlueprint", std::string(a),
			 bt.empty() ? std::string(p) : std::string(p) + "/" + std::string(bt));
		// Pretend it worked: return alreadyExisted=false, echo parent.
		CreateBlueprintResult r;
		r.alreadyExisted = false;
		r.parentClass = std::string(p);
		return r;
	}
	CloneGraphResult CloneGraph(std::string_view s, std::string_view t,
								std::string_view g) override {
		Note("CloneGraph", std::string(s), std::string(t) + ":" + std::string(g));
		CloneGraphResult r;
		r.ok = true;
		r.importedNodes = 0;
		return r;
	}
	void ImplementInterface(std::string_view a, std::string_view i) override {
		Note("ImplementInterface", std::string(a), std::string(i));
	}
	void SetPinDefault(std::string_view a, std::string_view, std::string_view,
					   std::string_view pin, std::string_view value) override {
		Note("SetPinDefault", std::string(a),
			 std::string(pin) + "=" + std::string(value));
	}
	void RetypeVariable(std::string_view a, std::string_view n, const BPPinType& t) override {
		Note("RetypeVariable", std::string(a),
			 std::string(n) + ":" + t.Category);
	}
	void SetVariableCategory(std::string_view a, std::string_view n, std::string_view c) override {
		Note("SetVariableCategory", std::string(a),
			 std::string(n) + "=" + std::string(c));
	}
	DuplicateBlueprintResult DuplicateBlueprint(std::string_view s, std::string_view d) override {
		Note("DuplicateBlueprint", std::string(s), std::string(d));
		DuplicateBlueprintResult r;
		r.alreadyExisted   = false;
		r.sourceAssetPath  = std::string(s);
		return r;
	}
	WriteGeneratedSourceResult WriteGeneratedSource(std::string_view p, std::string_view c, bool) override {
		Note("WriteGeneratedSource", std::string(p),
			 std::to_string(c.size()) + " bytes");
		WriteGeneratedSourceResult r;
		r.bytesWritten = c.size();
		r.path = std::string(p);
		return r;
	}

	// Batch sentinels — recorded but not forwarded; the inner mock has its
	// own no-op default that's fine for unit tests. EndBatch can return
	// a synthetic compile-diagnostics payload for C1 tests.
	void BeginBatch() override { ++beginBatchCalls; }
	nlohmann::json EndBatch(bool skipCompile = false) override {
		++endBatchCalls;
		if (skipCompile)
		{
			++endBatchSkipCalls;
		}
		return endBatchAck;
	}
	int endBatchSkipCalls = 0;
	nlohmann::json endBatchAck = nlohmann::json::object();

private:
	MockBlueprintReader inner_;
};

}    // namespace test_apply_ops_detail
using namespace test_apply_ops_detail;

TEST_CASE("apply_ops: empty ops list runs cleanly") {
	FakeWritableReader r;
	auto out = bpr::tools::RunOps(r, json::array(), /*atomic=*/true);
	CHECK(out["ok"] == true);
	CHECK(out["succeeded"] == 0);
	CHECK(out["failed"] == 0);
	CHECK(r.calls.empty());
}

TEST_CASE("apply_ops: sequential dispatch") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","NewVar"},
			 {"type","float"}},
		json{{"op","add_function"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","NewFunc"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	CHECK(out["succeeded"] == 2);
	REQUIRE(r.calls.size() == 2);
	CHECK(r.calls[0].op == "AddVariable");
	CHECK(r.calls[1].op == "AddFunction");
}

TEST_CASE("apply_ops emits one notifications/progress per op (PARITY-1 granular progress)") {
	FakeWritableReader r;
	jsonrpc::Server server;
	// Ambient call context with a progressToken — mirrors what the tools/call
	// dispatcher sets up. RunOps emits progress per op via CallContext::Current().
	jsonrpc::CallContext ctx(server, json(7001), std::optional<json>(json("ops-prog")));
	jsonrpc::CallContext::Scope scope(&ctx);

	json ops = json::array({
		json{{"op","add_variable"},{"asset_path","/Game/AI/BP_Enemy"},{"name","V1"},{"type","bool"}},
		json{{"op","add_variable"},{"asset_path","/Game/AI/BP_Enemy"},{"name","V2"},{"type","bool"}},
		json{{"op","add_variable"},{"asset_path","/Game/AI/BP_Enemy"},{"name","V3"},{"type","bool"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	CHECK(out["ok"] == true);

	auto notifs = server.TakePendingNotifications();
	int progress = 0;
	for (const auto& n : notifs) {
		if (n.value("method", std::string{}) == "notifications/progress" &&
			n["params"].value("progressToken", json()) == json("ops-prog")) {
			++progress;
		}
	}
	CHECK(progress == 3);  // one emit before each op
}

TEST_CASE("apply_ops: named slot resolves through wire_pins") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_node"}, {"id","branch"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","Branch"}, {"x",0},{"y",0}},
		json{{"op","add_node"}, {"id","seq"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","Sequence"}, {"x",200},{"y",0}},
		json{{"op","wire_pins"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"from_node","$branch"}, {"from_pin","then"},
			 {"to_node",  "$seq"},    {"to_pin",  "exec"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	CHECK(out["succeeded"] == 3);
	REQUIRE(out["slots"].size() == 2);
	CHECK(out["slots"].contains("branch"));
	CHECK(out["slots"].contains("seq"));
	// 3 writes = 2 add_node + 1 wire_pins. AddNode also triggers a
	// GetGraph for pin enrichment (a read), which doesn't show up in the
	// calls log.
	int adds = 0, wires = 0;
	for (const auto& c : r.calls) {
		if (c.op == "AddNode")
		{
			++adds;
		}
		if (c.op == "WirePins")
		{
			++wires;
		}
	}
	CHECK(adds  == 2);
	CHECK(wires == 1);
}

TEST_CASE("apply_ops: {ref:\"slot\"} form also resolves") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_node"}, {"id","n1"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","Branch"}, {"x",0},{"y",0}},
		json{{"op","delete_node"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"node_id", json{{"ref","n1"}}}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	CHECK(out["succeeded"] == 2);
}

TEST_CASE("apply_ops: unknown slot ref fails clearly") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","wire_pins"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"from_node","$does_not_exist"}, {"from_pin","exec"},
			 {"to_node",  "$also_missing"},   {"to_pin",  "exec"}},
	});
	CHECK_THROWS_AS(bpr::tools::RunOps(r, ops, /*atomic=*/true),
					bpr::backends::BlueprintReaderError);
}

TEST_CASE("apply_ops: cascade slot ref through failed op surfaces upstream cause (issue #8)") {
	// When an op that names a slot fails, downstream ops referencing
	// that slot get a richer error linking back to the upstream failure
	// — rather than a generic "slot has not been bound" message.
	FakeWritableReader r;
	r.failOnWrite = true;
	json ops = json::array({
		// Op 0: add_node — fails (failOnWrite), but binds slot $n1.
		json{{"op","add_node"}, {"id","n1"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","CallFunction"}, {"x", 0}, {"y", 0},
			 {"function_owner","KismetSystemLibrary"},
			 {"function_name","PrintString"}},
		// Op 1: wire_pins references $n1 — should fail with a message
		// pointing at op 0 as the cause, not just "slot not bound".
		json{{"op","wire_pins"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"from_node","$n1"}, {"from_pin","then"},
			 {"to_node",  "$n1"}, {"to_pin",  "exec"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/false);
	CHECK(out["ok"] == false);
	CHECK(out["failed"] == 2);
	REQUIRE(out["results"].size() == 2);
	// Op 1's error must mention the upstream op AND its slot.
	REQUIRE(out["results"][1].contains("error"));
	std::string err = out["results"][1]["error"].get<std::string>();
	CHECK(err.find("$n1") != std::string::npos);
	CHECK(err.find("earlier op") != std::string::npos);
	CHECK(out["results"][1].contains("cause"));
	CHECK(out["results"][1]["cause"] == "upstream-slot-failed");
}

TEST_CASE("apply_ops: atomic=false continues after failure") {
	FakeWritableReader r;
	r.failOnWrite = true;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V1"}, {"type","float"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V2"}, {"type","int"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/false);
	CHECK(out["ok"] == false);
	CHECK(out["succeeded"] == 0);
	CHECK(out["failed"] == 2);
	REQUIRE(out["results"].size() == 2);
	CHECK(out["results"][0]["ok"] == false);
	CHECK(out["results"][0].contains("error"));
}

TEST_CASE("apply_ops: idempotent add_variable returns already_existed:true") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Health"}, {"type","float"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	REQUIRE(out["results"].size() == 1);
	CHECK(out["results"][0]["ok"] == true);
	CHECK(out["results"][0]["already_existed"] == true);
	// And no AddVariable call was made — the duplicate was short-circuited.
	int adds = 0;
	for (const auto& c : r.calls)
	{
		if (c.op == "AddVariable") ++adds;
	}
	CHECK(adds == 0);
}

TEST_CASE("apply_ops: type shorthand flows through") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","SomeNewActor"}, {"type","object:Actor"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	CHECK(out["results"][0]["already_existed"] == false);
}

// ===== Batch sentinels (A1) ================================================

TEST_CASE("apply_ops: batches issue Begin/End once regardless of N ops") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V1"}, {"type","float"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V2"}, {"type","int"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V3"}, {"type","bool"}},
		json{{"op","add_node"}, {"id","n1"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","Branch"}, {"x",0},{"y",0}},
	});
	bpr::tools::RunOps(r, ops, true);
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
}

TEST_CASE("apply_ops: batch closes even on atomic failure mid-batch") {
	FakeWritableReader r;
	r.failOnWrite = true;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V1"}, {"type","float"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V2"}, {"type","int"}},
	});
	CHECK_THROWS(bpr::tools::RunOps(r, ops, /*atomic=*/true));
	// Best-effort failure semantics: EndBatch still flushed.
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
}

TEST_CASE("apply_ops: empty op list still opens+closes a batch (cheap no-op)") {
	FakeWritableReader r;
	bpr::tools::RunOps(r, json::array(), /*atomic=*/true);
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
}

// ===== create_blueprint (A3) ===============================================

TEST_CASE("apply_ops: create_blueprint forwards to backend with parent class") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","create_blueprint"},
			 {"asset_path","/Game/AI/BP_Generated"},
			 {"parent_class","Actor"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	CHECK(out["ok"] == true);
	REQUIRE(out["results"].size() == 1);
	auto& res = out["results"][0];
	CHECK(res["ok"] == true);
	CHECK(res["asset_path"] == "/Game/AI/BP_Generated");
	CHECK(res["parent_class"] == "Actor");
	int creates = 0;
	for (const auto& c : r.calls)
	{
		if (c.op == "CreateBlueprint") ++creates;
	}
	CHECK(creates == 1);
}

TEST_CASE("apply_ops: create_blueprint then add_variable in one batch") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","create_blueprint"},
			 {"asset_path","/Game/AI/BP_New"},
			 {"parent_class","Actor"}},
		// Same asset path — relies on AssetCreated registration so the
		// follow-up op can load the BP. The fake reader doesn't enforce
		// load order, but the call sequence is what we're asserting.
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_New"},
			 {"name","NewVar"},
			 {"type","float"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	CHECK(out["succeeded"] == 2);
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
}

// ===== preview_ops (B2) ====================================================

TEST_CASE("preview_ops: happy path doesn't touch the writable reader") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_node"}, {"id","n1"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"kind","Branch"}, {"x",0},{"y",0}},
		json{{"op","wire_pins"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"from_node","$n1"}, {"from_pin","then"},
			 {"to_node",  "$n1"}, {"to_pin",  "execute"}},
	});
	auto out = bpr::tools::ValidateOps(r, ops);
	CHECK(out["ok"] == true);
	CHECK(out["validated"] == 2);
	CHECK(out["failed"] == 0);
	REQUIRE(out["slots"].contains("n1"));
	REQUIRE(out["would_compile"].is_array());
	CHECK(out["would_compile"].size() == 1);
	CHECK(out["would_compile"][0] == "/Game/AI/BP_Enemy");
	// No write calls at all — preview is read-only.
	int writeCalls = 0;
	for (const auto& c : r.calls) {
		if (c.op != "ListVariables" && c.op != "ReadBlueprint")
		{
			++writeCalls;
		}
	}
	CHECK(writeCalls == 0);
	CHECK(r.beginBatchCalls == 0);
	CHECK(r.endBatchCalls   == 0);
}

TEST_CASE("preview_ops: rejects unbound slot reference") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","wire_pins"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","EventGraph"},
			 {"from_node","$nope"}, {"from_pin","exec"},
			 {"to_node",  "$alsonope"}, {"to_pin","exec"}},
	});
	auto out = bpr::tools::ValidateOps(r, ops);
	CHECK(out["ok"] == false);
	CHECK(out["failed"] == 1);
	REQUIRE(out["results"].size() == 1);
	CHECK(out["results"][0]["ok"] == false);
	CHECK(out["results"][0].contains("error"));
}

TEST_CASE("preview_ops: rejects bad type shorthand") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V"}, {"type","totally_garbage"}},
	});
	auto out = bpr::tools::ValidateOps(r, ops);
	CHECK(out["ok"] == false);
	CHECK(out["results"][0]["ok"] == false);
}

TEST_CASE("preview_ops: collects all affected asset paths") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V1"}, {"type","float"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/Items/BP_Pickup"},
			 {"name","V2"}, {"type","int"}},
	});
	auto out = bpr::tools::ValidateOps(r, ops);
	REQUIRE(out["would_compile"].is_array());
	CHECK(out["would_compile"].size() == 2);
}

// ===== C1: compile diagnostics surface in apply_ops result ================

TEST_CASE("apply_ops: surfaces diagnostics from EndBatch ack") {
	FakeWritableReader r;
	// Synthesize what the plugin's EndBatch would return after compiling
	// a BP that has a wire-type warning. RunOps lifts these to the top
	// level so the agent doesn't have to dig.
	r.endBatchAck = json{
		{"ok", true},
		{"recompiled", json::array({"/Game/AI/BP_Enemy"})},
		{"diagnostics", json::array({
			json{{"severity","warning"},
				 {"message","Implicit type conversion from int to bool"},
				 {"asset_path","/Game/AI/BP_Enemy"}}
		})},
		{"error_count",   0},
		{"warning_count", 1},
	};
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V1"}, {"type","float"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	CHECK(out["ok"] == true);
	REQUIRE(out.contains("diagnostics"));
	REQUIRE(out["diagnostics"].is_array());
	CHECK(out["diagnostics"].size() == 1);
	CHECK(out["diagnostics"][0]["severity"] == "warning");
	CHECK(out["compile_errors"]   == 0);
	CHECK(out["compile_warnings"] == 1);
	REQUIRE(out["recompiled"].is_array());
	CHECK(out["recompiled"].size() == 1);
}

TEST_CASE("apply_ops: empty diagnostics array is omitted on backends without compile") {
	FakeWritableReader r;
	// Default endBatchAck = {} → no diagnostics field set.
	auto out = bpr::tools::RunOps(r, json::array(), /*atomic=*/true);
	CHECK(out["ok"] == true);
	CHECK_FALSE(out.contains("diagnostics"));
	CHECK_FALSE(out.contains("compile_errors"));
}

// ===== on_failure flag (strict atomic mode) =================================

TEST_CASE("apply_ops: on_failure=skip flushes EndBatch with skipCompile=true on failure") {
	FakeWritableReader r;
	r.failOnWrite = true;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","NewVar"}, {"type","float"}},
	});
	CHECK_THROWS(bpr::tools::RunOps(r, ops, /*atomic=*/true, "skip"));
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
	CHECK(r.endBatchSkipCalls == 1);  // <-- skip path was taken
}

TEST_CASE("apply_ops: on_failure=compile (default) flushes EndBatch with skipCompile=false on failure") {
	FakeWritableReader r;
	r.failOnWrite = true;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","NewVar"}, {"type","float"}},
	});
	CHECK_THROWS(bpr::tools::RunOps(r, ops, /*atomic=*/true, "compile"));
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
	CHECK(r.endBatchSkipCalls == 0);  // <-- did NOT skip
}

TEST_CASE("apply_ops: on_failure=skip on success path still uses skipCompile=false") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Health"}, {"type","float"}},  // already exists, idempotent
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true, "skip");
	CHECK(out["ok"] == true);
	CHECK(r.endBatchSkipCalls == 0);  // success path doesn't honor skip
}

TEST_CASE("apply_ops: unknown on_failure value throws") {
	FakeWritableReader r;
	CHECK_THROWS_AS(
		bpr::tools::RunOps(r, json::array(), /*atomic=*/true, "yolo"),
		std::invalid_argument);
}

// ===== Op-attribution for compile diagnostics ===============================

TEST_CASE("apply_ops: tags diagnostics with op_index when node_guid was minted in this batch") {
	FakeWritableReader r;
	// Pre-arrange: stub the EndBatch ack to include a diagnostic pointing
	// at a node_guid that op[1]'s add_node will mint. The fake's Mint()
	// produces deterministic GUIDs starting at "00000000-...-000000000001".
	// op[0] = AddFunction → mints the function-entry GUID (000000000001)
	// op[1] = AddNode     → mints node GUID (000000000002)
	r.endBatchAck = json{
		{"ok", true},
		{"diagnostics", json::array({
			json{{"severity","warning"},
				 {"message","unused pin"},
				 {"node_guid","00000000-0000-0000-0000-000000000002"},
				 {"asset_path","/Game/AI/BP_Enemy"}}
		})},
		{"error_count", 0},
		{"warning_count", 1},
	};
	json ops = json::array({
		json{{"op","add_function"}, {"id","fn"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"name","NewFn"}},
		json{{"op","add_node"}, {"id","node"},
			 {"asset_path","/Game/AI/BP_Enemy"}, {"graph_name","NewFn"},
			 {"kind","Branch"}, {"x",0},{"y",0}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	REQUIRE(out["diagnostics"].is_array());
	REQUIRE(out["diagnostics"].size() == 1);
	auto& d = out["diagnostics"][0];
	REQUIRE(d.contains("op_index"));
	CHECK(d["op_index"] == 1);  // <-- minted by op[1] (add_node "node")
}

// ===== BP-2 / BP-5 / BP-7: retype, duplicate, category =====================

TEST_CASE("apply_ops: retype_variable forwards to backend with type shorthand") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","retype_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Item"},
			 {"type","object:Actor"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	int retypes = 0;
	for (const auto& c : r.calls)
	{
		if (c.op == "RetypeVariable") ++retypes;
	}
	CHECK(retypes == 1);
}

TEST_CASE("apply_ops: set_variable_category forwards label") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","set_variable_category"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Health"},
			 {"category","Stats"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	int cats = 0;
	for (const auto& c : r.calls)
	{
		if (c.op == "SetVariableCategory") ++cats;
	}
	CHECK(cats == 1);
}

TEST_CASE("apply_ops: set_variable_category empty category clears") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","set_variable_category"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Health"}},  // no category field
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
}

TEST_CASE("apply_ops: duplicate_blueprint returns dest path + already_existed") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","duplicate_blueprint"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"dest_asset_path","/Game/AI/BP_EnemyVariant"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	REQUIRE(out["results"].size() == 1);
	auto& res = out["results"][0];
	CHECK(res["asset_path"] == "/Game/AI/BP_EnemyVariant");
	CHECK(res["source_asset_path"] == "/Game/AI/BP_Enemy");
	CHECK(res["already_existed"] == false);
}

TEST_CASE("apply_ops: duplicate_blueprint then add_variable in one batch") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","duplicate_blueprint"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"dest_asset_path","/Game/AI/BP_NewVariant"}},
		json{{"op","add_variable"},
			 {"asset_path","/Game/AI/BP_NewVariant"},
			 {"name","NewField"}, {"type","int"}},
	});
	auto out = bpr::tools::RunOps(r, ops, true);
	CHECK(out["ok"] == true);
	CHECK(out["succeeded"] == 2);
	CHECK(r.beginBatchCalls == 1);
	CHECK(r.endBatchCalls   == 1);
}

TEST_CASE("preview_ops: duplicate_blueprint with non-/Game/ dest is rejected") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","duplicate_blueprint"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"dest_asset_path","/Engine/X/BP_Bad"}},
	});
	auto out = bpr::tools::ValidateOps(r, ops);
	CHECK(out["ok"] == false);
	CHECK(out["results"][0].contains("error"));
}

TEST_CASE("apply_ops: retype_variable validates type shorthand at the boundary") {
	FakeWritableReader r;
	json ops = json::array({
		json{{"op","retype_variable"},
			 {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","Item"},
			 {"type","totally_garbage"}},
	});
	CHECK_THROWS(bpr::tools::RunOps(r, ops, true));
}

TEST_CASE("apply_ops: diagnostic with unknown node_guid carries no op_index") {
	FakeWritableReader r;
	r.endBatchAck = json{
		{"ok", true},
		{"diagnostics", json::array({
			json{{"severity","warning"},
				 {"message","existing-node warning"},
				 {"node_guid","00000000-0000-0000-0000-deadbeefcafe"}}
		})},
	};
	json ops = json::array({
		json{{"op","add_variable"}, {"asset_path","/Game/AI/BP_Enemy"},
			 {"name","V"}, {"type","float"}},
	});
	auto out = bpr::tools::RunOps(r, ops, /*atomic=*/true);
	REQUIRE(out["diagnostics"].size() == 1);
	CHECK_FALSE(out["diagnostics"][0].contains("op_index"));
}
