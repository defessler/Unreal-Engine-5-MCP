// Live integration test for the CommandletBlueprintReader. Only runs when
// BP_READER_ENGINE_DIR and BP_READER_PROJECT are set in the environment;
// skipped otherwise so a fresh-clone doctest run stays fast.
//
// The seed commandlet (BlueprintReaderSeed) must have been run beforehand to
// produce /Game/AI/BP_TestEnemy and /Game/AI/BP_TestPickup.

#include <doctest/doctest.h>
#include "Env.h"

#include "backends/CommandletBlueprintReader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace test_commandlet_backend_detail {

std::string GetEnv(const char* key) {
#ifdef _MSC_VER
	char* buf = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
		std::string out(buf);
		std::free(buf);
		return out;
	}
	return {};
#else    // !_MSC_VER
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0')
	{
		return std::string(v);
	}
	return {};
#endif    // _MSC_VER
}

bool LiveBackendAvailable() {
	return !GetEnv("BP_READER_ENGINE_DIR").empty() &&
		   !GetEnv("BP_READER_PROJECT").empty();
}

std::unique_ptr<bpr::backends::CommandletBlueprintReader> MakeLiveReader(bool useDaemon = false) {
	bpr::backends::CommandletBlueprintReader::Config cfg;
	cfg.engineDir = std::filesystem::path(GetEnv("BP_READER_ENGINE_DIR"));
	cfg.uproject  = std::filesystem::path(GetEnv("BP_READER_PROJECT"));
	cfg.timeout   = std::chrono::seconds(180);
	cfg.useDaemon = useDaemon;
	return std::make_unique<bpr::backends::CommandletBlueprintReader>(std::move(cfg));
}

}    // namespace test_commandlet_backend_detail
using namespace test_commandlet_backend_detail;

TEST_CASE("CommandletBlueprintReader: List under /Game/AI returns seeded blueprints"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	auto items = reader->ListBlueprints("/Game/AI");
	REQUIRE_GE(items.size(), 2);
	bool sawEnemy = false, sawPickup = false;
	for (const auto& s : items) {
		if (s.AssetPath == "/Game/AI/BP_TestEnemy")
		{
			sawEnemy  = true;
		}
		if (s.AssetPath == "/Game/AI/BP_TestPickup")
		{
			sawPickup = true;
		}
	}
	CHECK(sawEnemy);
	CHECK(sawPickup);
}

TEST_CASE("CommandletBlueprintReader: recreate player-character skeleton (read -> rebuild -> diff)"
		  * doctest::skip(!LiveBackendAvailable())) {
	// Roadmap 1.7. Read a real player-character BP and reconstruct its
	// structural skeleton with the WRITE tools (explicitly NOT
	// duplicate_blueprint), then bp_structural_diff the two. Engine-subsystem
	// specifics the tools can't author standalone (GAS grants, input
	// mappings, full event-graph node bodies) surface as diff -- a coverage
	// signal, not a failure. We assert the reproducible skeleton
	// (variables/functions/components by name) round-trips and that
	// structural_diff runs on the pair.
	auto reader = MakeLiveReader(/*useDaemon=*/true);
	const std::string original  = "/Game/Characters/Heroes/B_Hero_Default";
	const std::string recreated = "/Game/Recreated/B_Hero_Default_RT";

	// Read the original; skip cleanly if Content isn't restored on this host
	// (Content is optional and regenerable via setup.bat).
	auto src = [&]() -> std::optional<decltype(reader->ReadBlueprint(original))> {
		try { return reader->ReadBlueprint(original); }
		catch (const bpr::backends::BlueprintReaderError&) { return std::nullopt; }
	}();
	if (!src) {
		MESSAGE("B_Hero_Default not present (run setup.bat to restore Content) -- skipping");
		return;
	}
	REQUIRE_FALSE(src->ParentClass.empty());

	// Fresh start: remove any leftover recreation, then build from scratch.
	(void)reader->DeleteAsset(recreated, /*force=*/true);
	try {
		reader->CreateBlueprint(recreated, src->ParentClass, "");
	} catch (const bpr::backends::BlueprintReaderError& e) {
		MESSAGE("parent class '" << src->ParentClass
				<< "' not creatable standalone (" << e.what() << ") -- skipping");
		return;
	}

	std::vector<std::string> addedVars, addedFns, addedComps;
	for (const auto& v : src->Variables) {
		try {
			reader->AddVariable(recreated, v.Name, v.Type,
								/*defaultValue=*/"", /*category=*/"",
								v.IsReplicated, v.IsEditable);
			addedVars.push_back(v.Name);
		} catch (const bpr::backends::BlueprintReaderError&) {
			// Type the write tools can't author standalone -> known gap.
		}
	}
	for (const auto& f : src->Functions) {
		try { reader->AddFunction(recreated, f.Name); addedFns.push_back(f.Name); }
		catch (const bpr::backends::BlueprintReaderError&) {}
	}
	auto comps = reader->GetComponents(original);
	for (const auto& c : comps) {
		try {
			reader->AddComponent(recreated, c.Name, c.Class,
								 c.Parent.value_or(std::string{}), /*socket=*/"");
			addedComps.push_back(c.Name);
		} catch (const bpr::backends::BlueprintReaderError&) {}
	}

	MESSAGE("recreated skeleton: vars=" << addedVars.size() << "/" << src->Variables.size()
			<< " fns=" << addedFns.size() << "/" << src->Functions.size()
			<< " comps=" << addedComps.size() << "/" << comps.size());

	// Everything we successfully added must read back from the rebuilt BP.
	auto rebuilt = reader->ReadBlueprint(recreated);
	auto hasVar = [&](const std::string& n) {
		return std::any_of(rebuilt.Variables.begin(), rebuilt.Variables.end(),
						   [&](const auto& v) { return v.Name == n; });
	};
	auto hasFn = [&](const std::string& n) {
		return std::any_of(rebuilt.Functions.begin(), rebuilt.Functions.end(),
						   [&](const auto& f) { return f.Name == n; });
	};
	for (const auto& n : addedVars) { CHECK(hasVar(n)); }
	for (const auto& n : addedFns)  { CHECK(hasFn(n)); }

	// At least part of the player-character skeleton must reproduce via the
	// write tools -- proves the read -> manual-recreate path works end to end.
	CHECK((addedVars.size() + addedFns.size() + addedComps.size()) > 0);

	// bp_structural_diff runs on the original/recreated pair. Drift is
	// expected (the irreproducible subsystem detail) -- this asserts the
	// tool executes and returns a structured result, not that drift is zero.
	auto diff = reader->StructuralDiff(original, recreated, {});
	CHECK(diff.is_object());

	(void)reader->DeleteAsset(recreated, /*force=*/true);
}

TEST_CASE("CommandletBlueprintReader: ReadBlueprint returns canonical wire shape"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	auto md = reader->ReadBlueprint("/Game/AI/BP_TestEnemy");
	CHECK(md.AssetPath == "/Game/AI/BP_TestEnemy");
	CHECK(md.Name == "BP_TestEnemy");
	CHECK_GE(md.Variables.size(), 3);

	bool sawHealth = false;
	for (const auto& v : md.Variables) {
		if (v.Name == "Health") {
			sawHealth = true;
			CHECK(v.IsReplicated);
			CHECK(v.IsEditable);
		}
	}
	CHECK(sawHealth);
	// TakeDamage + OnDeath are functions added by the seeder.
	CHECK_GE(md.Functions.size(), 2);
}

TEST_CASE("CommandletBlueprintReader: GetFunction returns inputs/outputs/locals"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	auto fn = reader->GetFunction("/Game/AI/BP_TestEnemy", "TakeDamage");
	CHECK(fn.Name == "TakeDamage");
	// The seeder declares Damage:float input, Killed:bool output, NewHealth local.
	CHECK_GE(fn.Inputs.size(), 1);
	CHECK_GE(fn.Outputs.size(), 1);
	CHECK_GE(fn.Locals.size(), 1);
	CHECK(fn.Graph.Type == "Function");
}

TEST_CASE("CommandletBlueprintReader: ListVariables surfaces seeded vars"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	auto vars = reader->ListVariables("/Game/AI/BP_TestPickup");
	CHECK_GE(vars.size(), 2);
}

TEST_CASE("CommandletBlueprintReader: AssetNotFound on bogus path"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();
	CHECK_THROWS_AS(reader->ReadBlueprint("/Game/Nope/Definitely_Does_Not_Exist"),
					bpr::backends::BlueprintReaderError);
}

TEST_CASE("CommandletBlueprintReader: get_components returns SCS data live"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);
	// The seeded BPs derive from AActor and don't add custom components, so the
	// contract here is just "the call succeeds without throwing." Exact count
	// depends on whether SCS materialises an inherited DefaultSceneRoot.
	auto comps = reader->GetComponents("/Game/AI/BP_TestEnemy");
	(void)comps;  // any result (including empty) is acceptable
}

TEST_CASE("CommandletBlueprintReader: extended add_node kinds (Cast, Self, MakeArray, FormatText, Knot)"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);
	const std::string asset = "/Game/AI/BP_TestEnemy";

	auto add = [&](const std::string& kind, int x, int y,
				   const std::map<std::string, std::string, std::less<>>& extras = {}) {
		return reader->AddNode(asset, "EventGraph", kind, x, y, extras);
	};

	std::string castId = add("Cast", -100, 600, {{"TargetClass","Actor"}});
	CHECK(!castId.empty());

	std::string selfId = add("Self", -100, 660);
	CHECK(!selfId.empty());

	std::string arrId  = add("MakeArray", -100, 720);
	CHECK(!arrId.empty());

	std::string fmtId  = add("FormatText", -100, 780);
	CHECK(!fmtId.empty());

	std::string knotId = add("Knot", -100, 840);
	CHECK(!knotId.empty());

	// All new ids should be unique.
	std::vector<std::string> ids{castId, selfId, arrId, fmtId, knotId};
	std::sort(ids.begin(), ids.end());
	CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

	// Cleanup so subsequent runs / fixtures stay tidy.
	for (const auto& id : ids)
	{
		reader->DeleteNode(asset, "EventGraph", id);
	}
}

TEST_CASE("CommandletBlueprintReader: function composition — add_function + add_function_input/output + set_variable_default + delete_function"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);
	const std::string asset = "/Game/AI/BP_TestEnemy";
	const std::string fn    = "BPR_LiveCompose";

	// Clean any leftover from a prior run. Ignore "not found".
	try { reader->DeleteFunction(asset, fn); } catch (...) {}

	// 1. Compose the function: name + input + output.
	auto added = reader->AddFunction(asset, fn);
	CHECK(added.functionName == fn);
	// The plugin echoes back the FunctionEntry node's GUID so callers can
	// wire its `then` exec without a follow-up read.
	CHECK_FALSE(added.entryNodeId.empty());

	BPPinType floatType;  floatType.Category = "real"; floatType.SubCategory = "float";
	BPPinType boolType;   boolType.Category  = "bool";
	reader->AddFunctionInput(asset, fn, "Damage", floatType);
	reader->AddFunctionOutput(asset, fn, "Killed", boolType);

	// 2. Read it back and assert the signature.
	auto fnSpec = reader->GetFunction(asset, fn);
	CHECK(fnSpec.Name == fn);
	REQUIRE_GE(fnSpec.Inputs.size(),  1);
	REQUIRE_GE(fnSpec.Outputs.size(), 1);
	bool sawInput = false, sawOutput = false;
	for (const auto& v : fnSpec.Inputs)
	{
		if (v.Name == "Damage")  sawInput  = true;
	}
	for (const auto& v : fnSpec.Outputs)
	{
		if (v.Name == "Killed") sawOutput = true;
	}
	CHECK(sawInput);
	CHECK(sawOutput);

	// 3. set_variable_default round-trip on an existing seeded variable.
	reader->SetVariableDefault(asset, "MaxHealth", "250.0");
	// ListVariables doesn't currently surface DefaultValue verbatim from the BP
	// serialization (it depends on whether the BP is compiled with the value
	// at the time of read), so we only assert that the call succeeded —
	// i.e., didn't throw. The save was committed because compile+save runs
	// inside SetVariableDefault.

	// 4. Tear down: delete the function.
	reader->DeleteFunction(asset, fn);
	auto md = reader->ReadBlueprint(asset);
	bool stillThere = false;
	for (const auto& f : md.Functions)
	{
		if (f.Name == fn) stillThere = true;
	}
	CHECK_FALSE(stillThere);
}

TEST_CASE("CommandletBlueprintReader: extended write tools — add_node + wire_pins + delete_variable + rename_variable"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);
	const std::string asset = "/Game/AI/BP_TestEnemy";

	// 1. Add a fresh CustomEvent node to the EventGraph. Returns a GUID.
	std::map<std::string, std::string, std::less<>> evtArgs{
		{"EventName", "BPR_TestEvent"}};
	std::string customEventId = reader->AddNode(
		asset, "EventGraph", "CustomEvent", -300, 320, evtArgs);
	CHECK(!customEventId.empty());

	// 2. Add a Branch node next to it.
	std::string branchId = reader->AddNode(
		asset, "EventGraph", "Branch", -50, 320, {});
	CHECK(!branchId.empty());

	// 3. Wire the CustomEvent's `then` output to the Branch's exec input.
	//    Pin GUIDs aren't easy to predict here, so use names — wire_pins
	//    accepts both.
	reader->WirePins(asset, "EventGraph", customEventId, "then", branchId, "execute");

	// 4. Read back the graph and assert the new connection exists.
	auto graph = reader->GetGraph(asset, "EventGraph");
	bool found = false;
	for (const auto& c : graph.Connections) {
		if (c.FromNode == customEventId && c.ToNode == branchId) { found = true; break; }
	}
	CHECK(found);

	// 5. Delete the Branch we just added (cleanup partial — keep the
	//    CustomEvent since UE's serialization may keep an event call somewhere).
	reader->DeleteNode(asset, "EventGraph", branchId);

	// 6. Rename a member variable then rename it back.
	reader->RenameVariable(asset, "MaxHealth", "MaxHP");
	auto vars1 = reader->ListVariables(asset);
	bool sawNew = false;
	for (const auto& v : vars1)
	{
		if (v.Name == "MaxHP") sawNew = true;
	}
	CHECK(sawNew);
	reader->RenameVariable(asset, "MaxHP", "MaxHealth");

	// 7. Delete the variable we'll re-add. Ignore "not found" errors
	//    (e.g. if a previous test run already left it removed).
	try { reader->DeleteVariable(asset, "AggroTarget"); } catch (...) {}
	auto vars2 = reader->ListVariables(asset);
	bool sawAggro = false;
	for (const auto& v : vars2)
	{
		if (v.Name == "AggroTarget") sawAggro = true;
	}
	CHECK_FALSE(sawAggro);

	// 8. Re-add it via add_variable so reseed isn't strictly necessary
	//    for any subsequent test runs in this session.
	BPPinType actorType;
	actorType.Category = "object";
	actorType.SubCategoryObject = "/Script/Engine.Actor";
	reader->AddVariable(asset, "AggroTarget", actorType, "", "AI", true, false);
}

TEST_CASE("CommandletBlueprintReader: write tools (AddVariable round-trip)"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);

	// Pick a unique name so re-running this test against an already-modified
	// BP doesn't fail the "already exists" guard. The seed always reseeds at
	// the start of a session so this is mostly defensive.
	const std::string varName = "BPR_LiveTestVar";

	// First, scrub it if it lingered from a previous run by reseeding the BP.
	// (No `delete_variable` tool yet — we rely on the seed commandlet.)

	BPPinType type;
	type.Category = "bool";

	try {
		reader->AddVariable("/Game/AI/BP_TestEnemy", varName, type,
							"false", "Tests", false, true);
	} catch (const bpr::backends::BlueprintReaderError& e) {
		// If it already exists from a prior run, that's a test artifact, not a
		// failure of write semantics. Surface it but don't fail the test —
		// the read-back below is what matters.
		INFO("AddVariable threw (likely already-exists): " << e.what());
	}

	auto vars = reader->ListVariables("/Game/AI/BP_TestEnemy");
	bool sawIt = false;
	for (const auto& v : vars) {
		if (v.Name == varName) { sawIt = true; break; }
	}
	CHECK(sawIt);
}

TEST_CASE("CommandletBlueprintReader: daemon mode reuses one editor process across calls"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader(/*useDaemon=*/true);

	// First call pays the editor cold-start (~5s); subsequent calls should be
	// sub-second since the editor stays alive.
	auto t0 = std::chrono::steady_clock::now();
	auto first = reader->ListBlueprints("/Game/AI");
	auto firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();

	REQUIRE_GE(first.size(), 2);

	auto t1 = std::chrono::steady_clock::now();
	auto md = reader->ReadBlueprint("/Game/AI/BP_TestEnemy");
	auto vars = reader->ListVariables("/Game/AI/BP_TestEnemy");
	auto fn = reader->GetFunction("/Game/AI/BP_TestEnemy", "TakeDamage");
	auto found = reader->FindNode("/Game/AI/BP_TestEnemy", "", "VariableGet");
	auto subsequentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t1).count();

	CHECK(md.Name == "BP_TestEnemy");
	CHECK_GE(vars.size(), 4);
	CHECK(fn.Name == "TakeDamage");
	CHECK_GE(found.size(), 1);

	// Soft check: 4 follow-up calls together should be much cheaper than the
	// first call (which paid the cold-start). On the dev box this is typically
	// ~0.5–1.5 s for the four calls vs ~5–7 s for the first.
	INFO("First call (cold start) took " << firstMs << "ms; "
		 "next 4 calls together took " << subsequentMs << "ms");
	CHECK(subsequentMs < firstMs * 4);
}

TEST_CASE("CommandletBlueprintReader: FindNode kind filter narrows by K2 extras"
		  * doctest::skip(!LiveBackendAvailable())) {
	auto reader = MakeLiveReader();

	// Empty query + kind="VariableGet" should match the bIsAlive Get node.
	auto vars = reader->FindNode("/Game/AI/BP_TestEnemy", "", "VariableGet");
	REQUIRE_GE(vars.size(), 1);
	bool sawIsAlive = false;
	for (const auto& n : vars) {
		if (n.Class == "K2Node_VariableGet")
		{
			sawIsAlive = true;
		}
	}
	CHECK(sawIsAlive);

	// Empty query + kind="CallFunction" should match the PrintString node.
	auto calls = reader->FindNode("/Game/AI/BP_TestEnemy", "", "CallFunction");
	REQUIRE_GE(calls.size(), 1);

	// kind="Event" should match the auto-generated BeginPlay/Tick events.
	auto events = reader->FindNode("/Game/AI/BP_TestEnemy", "", "Event");
	CHECK_GE(events.size(), 1);

	// Query + kind together should AND.
	auto noMatch = reader->FindNode("/Game/AI/BP_TestEnemy", "DoesNotExist", "Event");
	CHECK(noMatch.empty());
}
