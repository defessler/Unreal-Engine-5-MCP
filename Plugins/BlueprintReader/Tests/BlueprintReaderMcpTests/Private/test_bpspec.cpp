// Tests for the roundtrip library: BPSpec serialization, StableNodeId
// hashing, and the ReadToSpec / SpecToBP orchestrators that drive the
// existing IBlueprintReader interface.

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "roundtrip/BPSpec.h"

using namespace bpr::roundtrip;

TEST_CASE("BPSpec round-trips through JSON") {
	BPSpec s;
	s.package_path = "/Game/Foo/BP_Bar";
	s.parent_class = "/Script/Engine.Actor";
	s.interfaces   = { "/Game/Interfaces/BPI_Foo" };

	BPVariable v;
	v.Name = "Health";
	v.Type.Category = "float";
	v.DefaultValue = "100.0";
	s.variables.push_back(v);

	SpecComponent c;
	c.name = "Mesh";
	c.component_class = "/Script/Engine.StaticMeshComponent";
	c.parent_name = "Root";
	s.components.push_back(c);

	SpecFunction fn;
	fn.name = "DoStuff";
	fn.nodes.push_back(BPNode{ "n1", "K2Node_VariableSet", "Set Health" });
	s.functions.push_back(fn);

	s.event_graph.name = "EventGraph";
	s.event_graph.type = "EventGraph";
	s.event_graph.nodes.push_back(BPNode{ "e1", "K2Node_Event", "BeginPlay" });

	auto j = ToJson(s);
	BPSpec s2 = FromJson(j);

	CHECK(s2.package_path == s.package_path);
	CHECK(s2.parent_class == s.parent_class);
	REQUIRE(s2.variables.size() == 1);
	CHECK(s2.variables[0].Name == "Health");
	CHECK(s2.variables[0].Type.Category == "float");
	REQUIRE(s2.components.size() == 1);
	CHECK(s2.components[0].component_class == "/Script/Engine.StaticMeshComponent");
	REQUIRE(s2.functions.size() == 1);
	CHECK(s2.functions[0].name == "DoStuff");
	REQUIRE(s2.functions[0].nodes.size() == 1);
	CHECK(s2.functions[0].nodes[0].Class == "K2Node_VariableSet");
	REQUIRE(s2.event_graph.nodes.size() == 1);
}

TEST_CASE("StableNodeId is deterministic and content-sensitive") {
	BPNode a{ "guid-a", "K2Node_VariableGet", "Get Health" };
	a.Pins.push_back(BPPin{ "p1", "Health", "Output", BPPinType{ "float" } });
	BPNode b = a;
	b.Id = "guid-b";  // different GUID, same class+title+pins
	CHECK(StableNodeId(a, 0) == StableNodeId(b, 0));

	BPNode c = a;
	c.Title = "Get Mana";
	CHECK(StableNodeId(a, 0) != StableNodeId(c, 0));
}
