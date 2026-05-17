// BPIR — Blueprint Intermediate Representation.
//
// A versioned, documented JSON AST that any source-language frontend
// lowers to/from, and that maps 1:1 onto BP graph operations. The pivot
// for BP ↔ C++ ↔ (future) Lua / Python / JS conversions.
//
// BPIR is essentially compile_function's DSL formalized + extended. The
// existing DSL covers the common case (if/set/call/var/lit); BPIR adds
// the constructs C++ needs (return, cast, switch, loops, sequence,
// member, index, self, new_array, new_struct, plus an `unsupported`
// safety valve for BP constructs without a clean source mapping).
//
// Schema (top-level):
//   { "version": 1, "kind": "function"|"class", ...payload... }
//
// "function" payload:
//   { "name": "TakeDamage",
//     "metadata": { asset_path?, return_type?, ufunction_specifiers?[] },
//     "inputs":  [{name, type, ...?}],
//     "outputs": [{name, type, ...?}],
//     "locals":  [{name, type, ...?}],
//     "body":    [stmts...] }
//
// "class" payload:
//   { "name": "BP_Enemy",
//     "metadata": { asset_path?, parent_class?, uclass_specifiers?[] },
//     "interfaces": ["IDamageable", ...],
//     "variables": [{name, type, default?, category?, replicated?, editable?}],
//     "functions": [BPIR-function-doc, ...] }
//
// Statement forms:
//   {if: <expr>, then: [s], [else: [s]]}            // K2Node_IfThenElse
//   {set: "<varName>", to: <expr>, [scope: "local"|"member"|"input"|"output"]}
//   {call: "<fn>", [args: {pin: <expr>, ...}]}      // CallFunction
//   {comment: "<text>"}                             // node comment / region
//   {return: <expr> | [<expr>...] | null}           // FunctionResult
//   {cast: <expr>, to: "<class>",                   // K2Node_DynamicCast
//    [as: "<localName>"], success: [s], fail: [s]}
//   {switch: <expr>,                                // K2Node_Switch*
//    cases: {"<value>": [s], ...}, [default: [s]]}
//   {for_each: "<elemName>", in: <expr>, body: [s]} // ForEachLoop macro
//   {while: <expr>, body: [s]}                      // WhileLoop macro
//   {sequence: [[s1], [s2], ...]}                   // K2Node_ExecutionSequence
//   {break: null} | {continue: null}                // loop control
//   {broadcast: "<prop>", [target: <expr>],         // K2Node_CallDelegate
//    [args: {pin: <expr>, ...}]}
//   {bind_delegate: "<prop>", [target: <expr>],     // K2Node_AddDelegate
//    handler: "<fn>"}
//   {unbind_delegate: "<prop>", [target: <expr>],   // K2Node_RemoveDelegate
//    handler: "<fn>"}
//   {clear_delegate: "<prop>", [target: <expr>]}    // K2Node_ClearDelegate
//   {unsupported: {node_class, guid, reason, fields?}}  // safety valve
//
// Expression forms:
//   {var: "<name>", [scope: "local"|"member"|"input"|"output"]}
//   {lit: <value>}                                  // pin default (string|num|bool|null)
//   {call: "<fn>", [args: {pin: <expr>, ...}]}      // CallFunction return value
//   {cast: <expr>, to: "<class>"}                   // pure DynamicCast
//   {member: <expr>, name: "<field>"}               // BreakStruct / property
//   {index: <arr>, idx: <expr>}                     // Array_Get / Map_Find
//   {self: null}                                    // K2Node_Self
//   {new_array: [<expr>...]}                        // K2Node_MakeArray
//   {new_struct: "<type>", fields: {name: <expr>}}  // K2Node_MakeStruct
//   {new_set: [<expr>...]}                          // K2Node_MakeSet
//   {new_map: [{key: <expr>, value: <expr>}...]}    // K2Node_MakeMap
//
// Type strings reuse the shorthand grammar (tools/TypeShorthand.cpp):
// "float", "int", "object:Actor", "[]float", "{string:int}", etc.

#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools {

// Current schema version. Increment when adding a breaking change to
// any statement / expression form. Validators reject docs whose version
// is higher than this (we don't auto-downgrade), and migrate older docs
// silently up to current.
constexpr int kBpirSchemaVersion = 1;

// Validate a BPIR document. Throws std::invalid_argument with a
// structured message on any schema violation:
//   - missing required field at a specific path (e.g. "body[3].if")
//   - unknown statement / expression form
//   - wrong type for a known field
// Returns silently on success. Does not mutate.
void ValidateBpir(const nlohmann::json& doc);

// Top-level shape predicates — useful for callers that want to dispatch
// without throwing.
bool IsBpirFunction(const nlohmann::json& doc);
bool IsBpirClass(const nlohmann::json& doc);

// Set of recognized statement / expression keys. Exposed so callers
// (codegen / parsers) can reject unrecognized forms with consistent
// messages, and so adding a new form requires updating only this list.
const std::vector<std::string>& StatementForms();   // {"if","set","call","comment","return",...}
const std::vector<std::string>& ExpressionForms();  // {"var","lit","call","cast","member",...}

// Pick the form name from a statement / expression object. Returns the
// first key that matches one of the known forms, or empty string if
// none match. Used by ValidateBpir + codegen dispatchers.
std::string DetectStatementForm(const nlohmann::json& stmt);
std::string DetectExpressionForm(const nlohmann::json& expr);

// Migration: take a doc with version <= kBpirSchemaVersion and bring it
// up to current. Currently a no-op (we only have v1) but the seam is
// here so adding v2 doesn't require touching every consumer. Returns
// the migrated doc; the input is left unchanged.
nlohmann::json MigrateToCurrent(const nlohmann::json& doc);

}    // namespace bpr::tools
