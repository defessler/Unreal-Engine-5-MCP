#include "SpecToBP.h"

#include <exception>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace bpr::roundtrip {

namespace {
	bool DoStage(SpecToBPResult& res, std::string_view stage,
				 std::string_view op, std::function<void()> fn) {
		try { fn(); return true; }
		catch (const std::exception& e) {
			res.failing_stage   = std::string(stage);
			res.failing_op      = std::string(op);
			res.error_message   = e.what();
			return false;
		}
	}
}

SpecToBPResult SpecToBP(backends::IBlueprintReader& reader,
						const BPSpec& spec,
						std::string_view outputPackagePath) {
	SpecToBPResult res;
	res.output_package_path = std::string(outputPackagePath);

	// 1. CreateBlueprint — idempotent on backend side; rebuild starts here.
	if (!DoStage(res, "create", "CreateBlueprint", [&]{
		(void)reader.CreateBlueprint(outputPackagePath, spec.parent_class);
	})) return res;

	// 2. Variables
	for (const auto& v : spec.variables) {
		if (!DoStage(res, "variable", std::string("AddVariable:") + v.Name, [&]{
			reader.AddVariable(outputPackagePath, v.Name, v.Type,
							   v.DefaultValue.value_or(""),
							   v.Category.value_or(""),
							   v.IsReplicated, v.IsEditable);
		})) return res;
	}

	// 3. Components (root-first; topological sort by parent_name pointers).
	{
		std::vector<SpecComponent> ordered;
		std::map<std::string, bool> placed;
		auto canPlace = [&](const SpecComponent& c) {
			return c.parent_name.empty() || placed.count(c.parent_name);
		};
		auto remaining = spec.components;
		while (!remaining.empty()) {
			bool madeProgress = false;
			for (auto it = remaining.begin(); it != remaining.end();) {
				if (canPlace(*it)) {
					placed[it->name] = true;
					ordered.push_back(std::move(*it));
					it = remaining.erase(it);
					madeProgress = true;
				} else {
					++it;
				}
			}
			if (!madeProgress) {
				// Cycle or dangling parent: append rest unsorted so we still
				// try to materialize them; the backend will surface the
				// missing-parent error.
				for (auto& c : remaining) ordered.push_back(std::move(c));
				break;
			}
		}
		for (const auto& c : ordered) {
			if (!DoStage(res, "component", std::string("AddComponent:") + c.name, [&]{
				(void)reader.AddComponent(outputPackagePath, c.name,
										  c.component_class, c.parent_name, c.socket);
			})) return res;
			// Apply property overrides one by one.
			for (auto it = c.properties.begin(); it != c.properties.end(); ++it) {
				if (!DoStage(res, "component_property",
							 std::string("SetComponentProperty:") + c.name + "." + it.key(),
							 [&]{
					(void)reader.SetComponentProperty(outputPackagePath, c.name,
													  it.key(),
													  it.value().is_string()
														  ? it.value().get<std::string>()
														  : it.value().dump());
				})) return res;
			}
		}
	}

	// 4. Function skeletons (so callers' nodes can find them).
	for (const auto& f : spec.functions) {
		if (!DoStage(res, "function_skeleton", std::string("AddFunction:") + f.name, [&]{
			(void)reader.AddFunction(outputPackagePath, f.name);
		})) return res;
		for (const auto& in : f.inputs) {
			if (!DoStage(res, "function_input",
						 std::string("AddFunctionInput:") + f.name + "." + in.Name, [&]{
				reader.AddFunctionInput(outputPackagePath, f.name, in.Name, in.Type);
			})) return res;
		}
		for (const auto& out : f.outputs) {
			if (!DoStage(res, "function_output",
						 std::string("AddFunctionOutput:") + f.name + "." + out.Name, [&]{
				reader.AddFunctionOutput(outputPackagePath, f.name, out.Name, out.Type);
			})) return res;
		}
	}

	// 5. Nodes per graph (event graph + each function body + macros).
	auto materializeGraph = [&](const std::string& graphName,
								const std::vector<BPNode>& nodes,
								const std::vector<BPConnection>& conns) -> bool {
		// Map source stable IDs to new in-engine GUIDs returned by AddNode.
		std::map<std::string, std::string> idMap;
		for (const auto& n : nodes) {
			// Skip nodes that are auto-spawned by skeletons (FunctionEntry,
			// FunctionResult, etc.) — they'd collide. Leave their stable id
			// in the map so connections still resolve, even though we never
			// substitute in a real new id; downstream WirePins call against
			// the auto-spawned node would have to be routed by name, not
			// guid — accepted as a known limitation until plugin gains a
			// "find auto-spawned entry id" surface.
			const bool isAutoSpawn =
				n.Class == "K2Node_FunctionEntry" ||
				n.Class == "K2Node_FunctionResult";
			if (isAutoSpawn) {
				idMap[n.Id] = n.Id;
				continue;
			}

			// Translate the node class into AddNode's "kind" enum that the
			// backend understands. Add more kinds here as the backend's
			// dispatch grows.
			std::string kind;
			std::map<std::string, std::string, std::less<>> extras;
			if (n.Class == "K2Node_IfThenElse") { kind = "Branch"; }
			else if (n.Class == "K2Node_ExecutionSequence") { kind = "Sequence"; }
			else if (n.Class == "K2Node_VariableGet") {
				kind = "VariableGet";
				if (n.Meta.contains("variable")) extras["Variable"] = n.Meta["variable"].get<std::string>();
			}
			else if (n.Class == "K2Node_VariableSet") {
				kind = "VariableSet";
				if (n.Meta.contains("variable")) extras["Variable"] = n.Meta["variable"].get<std::string>();
			}
			else if (n.Class == "K2Node_CallFunction") {
				kind = "CallFunction";
				if (n.Meta.contains("function"))       extras["Function"]       = n.Meta["function"].get<std::string>();
				if (n.Meta.contains("function_owner")) extras["FunctionOwner"]  = n.Meta["function_owner"].get<std::string>();
				if (n.Meta.contains("target_class"))   extras["TargetClass"]    = n.Meta["target_class"].get<std::string>();
			}
			else if (n.Class == "K2Node_CustomEvent") {
				kind = "CustomEvent";
				if (n.Meta.contains("event_name")) extras["EventName"] = n.Meta["event_name"].get<std::string>();
			}
			else {
				res.failing_stage = "node_unsupported";
				res.failing_op    = std::string("AddNode:") + n.Class;
				res.error_message = std::string("SpecToBP: node class ") + n.Class +
									" not in AddNode dispatch; extend SpecToBP";
				return false;
			}

			std::string newId;
			if (!DoStage(res, "node", std::string("AddNode:") + n.Class, [&]{
				newId = reader.AddNode(outputPackagePath, graphName, kind,
									   n.Position.X, n.Position.Y, extras);
			})) return false;
			idMap[n.Id] = newId;
		}

		// Connections — best-effort: drop any whose endpoints we didn't
		// successfully translate (e.g. node-kind unsupported but caller
		// chose to continue past it via a future relaxed mode).
		for (const auto& c : conns) {
			auto fIt = idMap.find(c.FromNode);
			auto tIt = idMap.find(c.ToNode);
			if (fIt == idMap.end() || tIt == idMap.end()) continue;
			if (!DoStage(res, "connection", "WirePins", [&]{
				reader.WirePins(outputPackagePath, graphName,
								fIt->second, c.FromPin,
								tIt->second, c.ToPin);
			})) return false;
		}
		return true;
	};

	if (!materializeGraph(spec.event_graph.name.empty() ? "EventGraph"
													   : spec.event_graph.name,
						  spec.event_graph.nodes,
						  spec.event_graph.connections)) return res;
	for (const auto& f : spec.functions) {
		if (!materializeGraph(f.name, f.nodes, f.connections)) return res;
	}
	for (const auto& m : spec.macros) {
		if (!materializeGraph(m.name, m.nodes, m.connections)) return res;
	}

	res.ok = true;
	return res;
}

}    // namespace bpr::roundtrip
