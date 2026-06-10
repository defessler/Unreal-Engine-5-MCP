// K2NodeSkeletonEmit — compilable .h/.cpp skeleton generation for a custom
// UK2Node subclass (EDIT-5's `generate_k2node_skeleton` tool).
//
// Pure compute: takes a JSON spec (class name + pin spec + optional target
// function), returns header/source TEXT. Writes no files (that's the gated
// `write_generated_source` tool's job), touches no backend — works on every
// backend including mock.
//
// The emitted skeleton implements the canonical custom-node surface:
//   - AllocateDefaultPins()  — exec pins (unless pure) + one CreatePin per
//     pin-spec entry, with FCreatePinParams for containers
//   - GetNodeTitle / GetTooltipText / GetMenuCategory — LOCTEXT'd
//   - GetMenuActions          — the UBlueprintNodeSpawner registrar idiom
//   - IsNodePure              — from the spec's `pure` flag
//   - ExpandNode              — lowers to a UK2Node_CallFunction on the spec's
//     `target_function` (omitted entirely when no target is given — a node
//     that doesn't expand shouldn't override ExpandNode)
//
// Deliberate gaps surface as `notes` entries, not silent wrong code: object/
// struct sub-types the generator can't resolve to a StaticClass()/static
// struct get a TODO placeholder; the consuming module's Build.cs deps are
// listed; a class-less target_function gets FName-only binding advice.

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace bpr::tools {

struct K2NodeSkeletonEmitResult {
	std::string headerSource;
	std::string implSource;
	std::string headerFileName;   // suggested filename like "K2Node_MyNode.h"
	std::string implFileName;
	std::string className;        // resolved class name e.g. "UK2Node_MyNode"
	nlohmann::json notes = nlohmann::json::array();
};

// Emit the skeleton from a spec:
//   {
//     "class_name": "MyNode",            // required; sanitized; "K2Node_" prefix added if absent
//     "module_api": "MYMODULE",          // optional; emits MYMODULE_API
//     "title": "My Node",                // optional; default derived from class_name
//     "tooltip": "...",                  // optional
//     "menu_category": "Custom",         // optional
//     "pure": false,                     // optional; true = no exec pins + IsNodePure()=true
//     "pins": [                          // optional
//       { "name": "Value", "direction": "input"|"output",
//         "category": "bool|byte|int|int64|float|name|string|text|object|class|
//                      softobject|softclass|struct|wildcard",
//         "sub_object": "/Script/Engine.Actor",   // optional, object/class/struct subtype
//         "container": "none|array|set|map",      // optional
//         "default_value": "0.0",                 // optional
//         "is_reference": false }                 // optional
//     ],
//     "target_function": "/Script/MyModule.MyFunctionLibrary:DoThing"  // optional
//   }
// Throws std::invalid_argument with a clear message on bad input.
K2NodeSkeletonEmitResult EmitK2NodeSkeleton(const nlohmann::json& spec);

}    // namespace bpr::tools
