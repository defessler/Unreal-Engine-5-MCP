// CompileFunction — translate a tiny pseudocode DSL into a sequence of
// add_node + wire_pins ops, then run them through the same path as
// apply_ops. Lets the agent think in pseudocode (its native form) and
// have the server materialize the graph.
//
// DSL (informal):
//
//   { "asset_path": "/Game/AI/BP_Enemy",
//     "function_name": "TakeDamage",
//     "inputs":  [ { "name": "Amount", "type": "float" } ],
//     "body": [
//       { "if": { "var": "bIsInvulnerable" },
//         "then": [],
//         "else": [
//           { "set": "Health",
//             "to": { "sub": [ { "var": "Health" }, { "var": "Amount" } ] } }
//         ]
//       }
//     ]
//   }
//
// Statement forms supported in v1:
//   { "if":   <expr>, "then": [stmts], "else": [stmts] }
//   { "set":  "<varName>", "to": <expr> }
//   { "call": "<functionName>", "args": { name: <expr>, ... } }
//   { "comment": "<string>" }       // attaches as a node comment near the entry
//
// Expression forms supported in v1:
//   { "var": "<varName>" }                    -> VariableGet
//   { "lit": <value> }                        -> literal pin default (string form)
//   { "call": "<fn>", "args": { ... } }       -> CallFunction whose return pin is the value
//
// Anything else is a v2 task. Out-of-scope statements/expressions throw a
// helpful error pointing at exactly which form was unrecognized — the
// agent then falls back to apply_ops.
//
// The compiler:
//   1. Adds the function (idempotent) + its inputs.
//   2. Lays nodes out in a simple top-down stack so they don't overlap.
//   3. For each statement, materializes the necessary nodes and wires
//      exec + data pins. Tracks the "current exec tail" — the pin we'd
//      hook the next sequential statement to.
//   4. Runs everything through apply_ops semantics so a single tool
//      call covers it.
//
// Limitation: the function-entry node and FunctionResult node need pin
// names matching the param names on the function. If a param name from
// the DSL doesn't map cleanly onto a real entry-pin, wire_pins surfaces
// the actual pin types in its error so the agent can self-correct.

#pragma once

#include "tools/ToolRegistry.h"
#include "backends/IBlueprintReader.h"

namespace bpr::tools {

void RegisterCompileFunction(ToolRegistry& registry,
                             backends::IBlueprintReader& reader);

}    // namespace bpr::tools
