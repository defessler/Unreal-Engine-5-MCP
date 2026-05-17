// Decompile — convert BP graphs to BPIR (the inverse of compile_function).
//
// Walks the BPGraph already returned by IBlueprintReader::GetFunction and
// reconstructs a structured BPIR AST by pattern-matching on K2 node
// classes. Server-side only — all the K2 metadata we need is already
// in BPNode.meta thanks to BlueprintIntrospector.
//
// Algorithm:
//   1. Find the FunctionEntry node; take its `then` exec output as start.
//   2. Walk exec edges; classify each node by its Class field.
//   3. Pattern-match recognized control flow (Branch, Sequence, Cast,
//      Switch, MacroInstance for ForEach/While) into BPIR. Recurse into
//      branches; converge at the immediate post-dominator.
//   4. For value-shaped nodes (VariableGet, CallFunction-as-rvalue,
//      MakeArray, MakeStruct, Self, Literal), trace data edges backward
//      from each consumer pin to build expressions.
//   5. Anything that doesn't pattern-match → emit `{unsupported}` with
//      the node's class + guid + relevant meta. Lossless: callers see
//      exactly what couldn't be represented.
//
// Cleanly supported: if/then/else, set/call/return, cast, sequence,
// var/lit/call expressions, self.
//
// Emitted as `{unsupported}`: Switch nodes, ForEachLoop/WhileLoop/DoOnce
// macros, timelines, async actions, latent — domain-specific, not
// portable. Any unknown node class.
//
// Best-effort: branches whose then/else don't reconverge (one returns) —
// emitted as an if without an else-tail; callers handle divergence.
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

}    // namespace bpr::tools
