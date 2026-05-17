// BPSpec — a complete structural snapshot of a Blueprint, formatted as the
// pivot between ReadToSpec (drives read tools) and SpecToBP (drives write
// tools). Self-contained: serializes to JSON without referencing live UObject
// state.
//
// Wire format: snake_case JSON, same convention as BlueprintReaderTypes.h.
// Node IDs are stable content-hashes (NOT FGuid) so a freshly-rebuilt BP can
// be matched against the source without GUID equality.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "BlueprintReaderTypes.h"

namespace bpr::roundtrip {

struct SpecFunction {
	std::string name;
	std::vector<BPPin> inputs;
	std::vector<BPPin> outputs;
	std::vector<BPVariable> locals;
	std::vector<BPNode> nodes;
	std::vector<BPConnection> connections;
};

struct SpecGraph {
	std::string name;
	std::string type;       // "EventGraph", "Macro", "Function", "Construction"
	std::vector<BPNode> nodes;
	std::vector<BPConnection> connections;
};

struct SpecComponent {
	std::string name;
	std::string component_class;
	std::string parent_name;  // empty = root
	std::string socket;
	nlohmann::json properties = nlohmann::json::object();  // serialized overrides
};

struct BPSpec {
	std::string package_path;
	std::string parent_class;
	std::vector<std::string> interfaces;
	std::vector<BPVariable> variables;
	std::vector<SpecComponent> components;
	std::vector<SpecFunction> functions;
	std::vector<SpecGraph> macros;
	SpecGraph event_graph;
	bool incomplete = false;
	std::vector<std::string> errors;
};

// JSON glue.
nlohmann::json ToJson(const BPSpec& spec);
BPSpec FromJson(const nlohmann::json& j);

// Generate a content-hash node id (stable across rebuilds for nodes that
// have the same class + signature + position-rank). Returns hex digits.
std::string StableNodeId(const BPNode& node, std::size_t positionRank);

}    // namespace bpr::roundtrip
