// Decompile — convert BP graphs to BPIR (Phase 1B of the BP↔C++ plan).
//
// Walks the BPGraph already returned by IBlueprintReader::GetFunction and
// reconstructs a structured BPIR AST by pattern-matching on K2 node
// classes. Server-side only — all the K2 metadata we need is already
// in BPNode.meta thanks to BlueprintIntrospector.
//
// Algorithm (high level):
//   1. Find the FunctionEntry node; take its `then` exec output as start.
//   2. Walk exec edges; for each node, classify by Class field.
//   3. Pattern-match recognized control-flow nodes (Branch, Sequence,
//      Cast, Switch, MacroInstance for ForEach/While) to their BPIR
//      counterparts. Recurse into branches; converge at the immediate
//      post-dominator.
//   4. For value-shaped nodes (VariableGet, CallFunction-as-rvalue,
//      MakeArray, MakeStruct, Self, Literal), trace data edges backward
//      from each consumer pin to build expressions.
//   5. Anything that doesn't match a known pattern → emit `{unsupported}`
//      with the node's class + guid + relevant meta. Lossless: callers
//      can see exactly what couldn't be represented.
//
// What v1 supports cleanly:
//   - if/then/else (K2Node_IfThenElse with both branches reconverging)
//   - set / call / return (VariableSet, CallFunction, FunctionResult)
//   - cast (DynamicCast with success + fail)
//   - sequence (ExecutionSequence)
//   - var/lit/call expressions (VariableGet, K2Node_Literal, CallFunction-rvalue)
//   - self (K2Node_Self)
//
// What v1 emits as `{unsupported}`:
//   - Switch nodes (K2Node_Switch* — pattern catalog grows over time)
//   - Macros (ForEachLoop, WhileLoop, DoOnce — pattern-match by macro name)
//   - Timelines, async actions, latent — domain-specific, not portable
//   - Any node class we don't recognize
//
// What v1 does best-effort:
//   - Branches whose then/else don't reconverge (one returns) — we
//     emit the if without an else-tail, callers handle the divergence.
//
// The output is a BPIR `{kind: "function", ...}` document validated by
// tools::ValidateBpir before return.

#pragma once

#include "BlueprintReaderTypes.h"
#include "backends/IBlueprintReader.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace bpr::tools {

// Decompile a single function from a blueprint into BPIR form. Reads
// `BPMetadata` (for variable scoping) + `BPFunction` from `reader`,
// runs the AST reconstruction pass, validates, and returns.
//
// Throws BlueprintReaderError if the asset/function can't be read, or
// if the resulting BPIR fails validation (would only happen on a bug
// in the decompile pass — the validator's role here is a sanity check).
nlohmann::json DecompileFunction(backends::IBlueprintReader& reader,
                                 std::string_view assetPath,
                                 std::string_view functionName);

// Decompile every function on a blueprint plus its variable list,
// returning a BPIR `{kind:"class", ...}` document. Used by
// `decompile_blueprint` for whole-class C++ generation.
nlohmann::json DecompileBlueprint(backends::IBlueprintReader& reader,
                                  std::string_view assetPath);

} // namespace bpr::tools
