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

	// Read a meta field tolerating both the camelCase variant emitted by the
	// live plugin (e.g. `variableName`, `eventName`, `targetFunction`) AND
	// the snake_case variants found in mock-backend fixtures (`variable`,
	// `variable_reference`, `event_name`, `function`, etc.). Returns the
	// first non-empty match in the candidate list. `meta` is the BPNode::Meta
	// JSON object.
	std::string MetaString(const nlohmann::json& meta,
						   std::initializer_list<const char*> candidates) {
		if (!meta.is_object()) return {};
		for (const char* key : candidates) {
			auto it = meta.find(key);
			if (it == meta.end()) continue;
			if (it->is_string()) {
				const auto v = it->get<std::string>();
				if (!v.empty()) return v;
			}
		}
		return {};
	}

	// Look up the auto-spawned function-skeleton node (FunctionEntry or
	// FunctionResult) in the rebuilt clone via FindNode + post-filter by
	// graph name. Returns the node's GUID, or empty if not found.
	std::string FindSkeletonNodeGuid(backends::IBlueprintReader& reader,
									 const std::string& assetPath,
									 const std::string& graphName,
									 const std::string& kind) {
		try {
			auto matches = reader.FindNode(assetPath, /*query=*/"", kind);
			for (const auto& n : matches) {
				if (n.GraphName.has_value() && *n.GraphName == graphName) {
					return n.Id;
				}
			}
		} catch (const std::exception&) {
			// FindNode shouldn't fail for valid assets, but swallow and let
			// the caller fall back to leaving the id unmapped.
		}
		return {};
	}

	// Look up an auto-spawned event node (e.g. ReceiveBeginPlay) by event
	// name. Most Actor-derived BPs auto-spawn lifecycle events at create
	// time; SpecToBP doesn't (and shouldn't) try to re-create them. Returns
	// the clone-side GUID, or empty if not found.
	std::string FindEventNodeGuid(backends::IBlueprintReader& reader,
								  const std::string& assetPath,
								  const std::string& graphName,
								  const std::string& eventName) {
		if (eventName.empty()) return {};
		try {
			auto matches = reader.FindNode(assetPath, eventName, "Event");
			for (const auto& n : matches) {
				if (!n.GraphName.has_value() || *n.GraphName != graphName) continue;
				// FindNode matches by case-insensitive substring; double-check
				// the event-name meta exact-equals the requested one.
				const std::string actualName = MetaString(n.Meta, {"eventName", "event_name"});
				if (actualName == eventName) {
					return n.Id;
				}
			}
		} catch (const std::exception&) {
			// Same as above — swallow and let the caller treat as missing.
		}
		return {};
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

	// 4. Function skeletons (so callers' nodes can find them). Capture the
	// FunctionEntry GUID returned by AddFunction so materializeGraph can
	// substitute the source-spec's stable FunctionEntry id with the real
	// clone-side guid (otherwise WirePins against the entry's `then` pin
	// drops on the floor and the function-graph link_count drifts).
	std::map<std::string, std::string> functionEntryGuids;   // fn name -> guid
	for (const auto& f : spec.functions) {
		if (!DoStage(res, "function_skeleton", std::string("AddFunction:") + f.name, [&]{
			auto added = reader.AddFunction(std::string(outputPackagePath), f.name);
			if (!added.entryNodeId.empty()) {
				functionEntryGuids[f.name] = added.entryNodeId;
			}
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
		// Map source stable IDs to new in-engine GUIDs returned by AddNode
		// (or to the GUID of an auto-spawned skeleton/event node we found
		// via FindNode on the clone).
		std::map<std::string, std::string> idMap;
		for (const auto& n : nodes) {
			// K2Node_FunctionEntry and K2Node_FunctionResult are spawned by
			// the engine the moment a function graph is created (entry) or
			// the first AddFunctionOutput call (result). We can't AddNode
			// over them; instead, resolve the live GUID via the entry-id
			// returned from AddFunction (or via FindNode for the result).
			if (n.Class == "K2Node_FunctionEntry") {
				std::string guid;
				if (auto it = functionEntryGuids.find(graphName);
					it != functionEntryGuids.end()) {
					guid = it->second;
				}
				if (guid.empty()) {
					guid = FindSkeletonNodeGuid(reader,
												std::string(outputPackagePath),
												graphName, "FunctionEntry");
				}
				if (guid.empty()) {
					// Couldn't find it — fall back to the source id so
					// downstream connections still map to *something*.
					// Inevitable diff fallout (auto-spawn-id known gap).
					guid = n.Id;
				}
				idMap[n.Id] = guid;
				continue;
			}
			if (n.Class == "K2Node_FunctionResult") {
				std::string guid = FindSkeletonNodeGuid(reader,
														std::string(outputPackagePath),
														graphName, "FunctionResult");
				if (guid.empty()) guid = n.Id;
				idMap[n.Id] = guid;
				continue;
			}
			// K2Node_Event covers Actor's auto-spawned lifecycle hooks
			// (ReceiveBeginPlay, ReceiveTick, etc.) — UBlueprintFactory's
			// FactoryCreateNew installs them up-front for Actor-derived
			// parents. SpecToBP can't "add" them because they already exist
			// in the freshly-created clone; find the existing node and
			// route the source's stable id at it instead.
			if (n.Class == "K2Node_Event") {
				const std::string eventName =
					MetaString(n.Meta, {"eventName", "event_name"});
				std::string guid = FindEventNodeGuid(reader,
													 std::string(outputPackagePath),
													 graphName, eventName);
				if (!guid.empty()) {
					idMap[n.Id] = guid;
					continue;
				}
				// Falling through means the engine didn't auto-spawn this
				// event (e.g. ReceiveTick on a BP that doesn't enable
				// ticking, or a custom-class event). Today there is no
				// public AddNode kind for K2Node_Event; surface a structured
				// failure so a future iteration can add one.
				res.failing_stage = "node_unsupported";
				res.failing_op    = std::string("AddNode:K2Node_Event(") + eventName + ")";
				res.error_message = std::string("SpecToBP: event '") + eventName +
									"' not auto-spawned on clone and no AddNode "
									"kind exists for K2Node_Event yet";
				return false;
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
				const auto v = MetaString(n.Meta,
					{"variableName", "variable_reference", "variable", "variable_name"});
				if (!v.empty()) extras["Variable"] = v;
			}
			else if (n.Class == "K2Node_VariableSet") {
				kind = "VariableSet";
				const auto v = MetaString(n.Meta,
					{"variableName", "variable_reference", "variable", "variable_name"});
				if (!v.empty()) extras["Variable"] = v;
			}
			else if (n.Class == "K2Node_CallFunction") {
				kind = "CallFunction";
				const auto fn = MetaString(n.Meta,
					{"targetFunction", "function_name", "function"});
				if (!fn.empty()) extras["Function"] = fn;
				// Owner: live plugin emits `targetClass` (full UClass path);
				// mock fixtures use `function_owner` (short name). Send the
				// path-form to the plugin so its ResolveClass can match
				// either; mock backend tolerates either format too.
				const auto owner = MetaString(n.Meta,
					{"targetClass", "function_owner", "target_class"});
				if (!owner.empty()) extras["FunctionOwner"] = owner;
			}
			else if (n.Class == "K2Node_CustomEvent") {
				kind = "CustomEvent";
				const auto en = MetaString(n.Meta, {"eventName", "event_name"});
				if (!en.empty()) extras["EventName"] = en;
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

		// Build a lookup from source-pin GUID -> pin name, walking each
		// node's pin list once. WirePins on the clone has to be passed
		// pin NAMES (not GUIDs), because the clone's pin GUIDs differ
		// from the source's even when the pin's logical name ("then",
		// "Condition", etc.) is identical. FindPinByIdOrName on the
		// plugin side falls through to a case-insensitive FName match
		// when the GUID parse succeeds but doesn't resolve to a pin —
		// but only if the spec we send IS that name. Sending the
		// source's pin GUID never matches anything on the clone.
		std::map<std::string, std::string> pinNameByGuid;
		for (const auto& n : nodes) {
			for (const auto& p : n.Pins) {
				pinNameByGuid[p.Id] = p.Name;
			}
		}
		auto resolvePinSpec = [&](const std::string& guidSpec) -> std::string {
			auto it = pinNameByGuid.find(guidSpec);
			if (it != pinNameByGuid.end() && !it->second.empty()) {
				return it->second;
			}
			// Fall back to the raw spec — covers the rare case where
			// the source's connection used a pin name directly (the
			// introspector emits a name fallback when the pin had no
			// captured id) and lets plugin-side name-match still hit.
			return guidSpec;
		};

		// Connections — best-effort: drop any whose endpoints we didn't
		// successfully translate, AND swallow per-connection WirePins
		// failures so a single broken wire (e.g. id we mapped via
		// FindNode no longer points at a live node because the auto-
		// spawned-event compile-pass rebuilt the graph) doesn't halt
		// the rest of the rebuild. The diff against the source will
		// surface any missed links as a link_count drift, which the
		// test exemption can pick up as a known-gap case.
		for (const auto& c : conns) {
			auto fIt = idMap.find(c.FromNode);
			auto tIt = idMap.find(c.ToNode);
			if (fIt == idMap.end() || tIt == idMap.end()) continue;
			const std::string fromPinSpec = resolvePinSpec(c.FromPin);
			const std::string toPinSpec   = resolvePinSpec(c.ToPin);
			try {
				reader.WirePins(outputPackagePath, graphName,
								fIt->second, fromPinSpec,
								tIt->second, toPinSpec);
			} catch (const std::exception&) {
				// best-effort — keep going.
			}
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
