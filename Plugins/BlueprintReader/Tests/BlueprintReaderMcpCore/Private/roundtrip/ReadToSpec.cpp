#include "ReadToSpec.h"

#include <exception>
#include <map>
#include <string>
#include <utility>

namespace bpr::roundtrip {

namespace {
	template <class Fn>
	void Try(BPSpec& spec, std::string_view stage, Fn&& fn) {
		try { fn(); }
		catch (const std::exception& e) {
			spec.incomplete = true;
			spec.errors.emplace_back(std::string(stage) + ": " + e.what());
		}
	}
}

BPSpec ReadToSpec(backends::IBlueprintReader& reader, std::string_view assetPath) {
	BPSpec spec;
	spec.package_path = std::string(assetPath);

	BPMetadata meta;
	Try(spec, "ReadBlueprint", [&]{
		meta = reader.ReadBlueprint(assetPath);
		spec.parent_class = meta.ParentClass;
		spec.interfaces   = meta.Interfaces;
	});

	Try(spec, "ListVariables", [&]{
		spec.variables = reader.ListVariables(assetPath);
	});

	Try(spec, "GetComponents", [&]{
		auto comps = reader.GetComponents(assetPath);
		for (const auto& c : comps) {
			SpecComponent sc;
			sc.name = c.Name;
			sc.component_class = c.Class;
			// Parent is optional on BPComponent; empty parent_name => root.
			sc.parent_name = c.Parent.value_or("");
			// Socket is not surfaced by the current wire shape — record empty.
			sc.socket = "";
			// Properties: capture each override as a {name: value_text} map
			// so SpecToBP can replay them via SetComponentProperty.
			nlohmann::json props = nlohmann::json::object();
			for (const auto& ov : c.Properties) {
				props[ov.Name] = ov.ValueText;
			}
			sc.properties = std::move(props);
			spec.components.push_back(std::move(sc));
		}
	});

	// Functions: one GetFunction call gives us the signature + locals +
	// the function body graph in a single payload.
	for (const auto& fnSummary : meta.Functions) {
		SpecFunction fn;
		fn.name = fnSummary.Name;
		Try(spec, std::string("GetFunction(") + fn.name + ")", [&]{
			auto fnInfo = reader.GetFunction(assetPath, fn.name);
			fn.inputs  = fnInfo.Inputs;
			fn.outputs = fnInfo.Outputs;
			fn.locals  = fnInfo.Locals;
			fn.nodes       = fnInfo.Graph.Nodes;
			fn.connections = fnInfo.Graph.Connections;
		});
		spec.functions.push_back(std::move(fn));
	}

	// Macros: graph-only. BPMetadata.Macros is a vector<string> of names.
	for (const auto& macroName : meta.Macros) {
		SpecGraph g;
		g.name = macroName;
		g.type = "Macro";
		Try(spec, std::string("GetGraph(") + g.name + ")", [&]{
			auto graph = reader.GetGraph(assetPath, g.name);
			g.nodes       = graph.Nodes;
			g.connections = graph.Connections;
		});
		spec.macros.push_back(std::move(g));
	}

	// Event graph (canonical name: "EventGraph"; the construction script
	// under "UserConstructionScript" / "ConstructionScript" is surfaced
	// separately via the graphs[] list — we follow that here for any
	// non-function, non-macro graph the BP declares).
	Try(spec, "GetGraph(EventGraph)", [&]{
		auto g = reader.GetGraph(assetPath, "EventGraph");
		spec.event_graph.name = g.Name;
		spec.event_graph.type = g.Type;
		spec.event_graph.nodes = g.Nodes;
		spec.event_graph.connections = g.Connections;
	});

	// Reassign stable node IDs across all graphs so the diff tool can
	// match nodes between source and clone.
	auto restamp = [](std::vector<BPNode>& nodes,
					  std::vector<BPConnection>& conns) {
		std::map<std::string, std::string> idMap;
		for (std::size_t i = 0; i < nodes.size(); ++i) {
			auto stable = StableNodeId(nodes[i], i);
			idMap[nodes[i].Id] = stable;
			nodes[i].Id = stable;
		}
		for (auto& c : conns) {
			auto fIt = idMap.find(c.FromNode);
			if (fIt != idMap.end()) c.FromNode = fIt->second;
			auto tIt = idMap.find(c.ToNode);
			if (tIt != idMap.end()) c.ToNode = tIt->second;
		}
	};
	for (auto& f : spec.functions)  restamp(f.nodes, f.connections);
	for (auto& m : spec.macros)     restamp(m.nodes, m.connections);
	restamp(spec.event_graph.nodes, spec.event_graph.connections);

	return spec;
}

}    // namespace bpr::roundtrip
