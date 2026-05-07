// Type shorthand: accept either the canonical BPPinType object form or a
// string shortcut, and normalize to BPPinType.
//
// Why: every write tool that takes a type used to require this:
//   { "type": { "category": "real", "sub_category": "float" } }
// AI clients pay tokens per byte and reasoning effort per shape. With
// shorthand:
//   { "type": "float" }
//   { "type": "object:Actor" }
//   { "type": "[]float" }
//   { "type": "{string:int}" }
//
// Grammar (informal):
//   <type>     ::= <container> <element>
//   <container> ::= "" | "[]" | "{}"  (array, set)
//   <map>      ::= "{" <key> ":" <value> "}"
//   <element>  ::= <name> | <name> ":" <subref>
//   <name>     ::= "bool" | "byte" | "int" | "int64"
//                | "float" | "real" | "double"
//                | "string" | "name" | "text"
//                | "object" | "class" | "struct" | "enum" | "interface"
//                | "exec"
//   <subref>   ::= UE class/struct/enum path or short name
//
// `real` and `float` both map to {category:"real", sub_category:"float"}
// to match UE5.7's pin schema (where "float" is a sub-category of "real").
//
// Object-ref shorthand:
//   "object:Actor"                          -> object reference to AActor
//   "object:/Game/AI/MyClass.MyClass_C"     -> long path supported
// Struct shorthand:
//   "struct:FVector"                        -> struct value
//   "struct:Vector"                         -> short name accepted
// Container shorthand:
//   "[]float"                               -> array<float>
//   "{}int"                                 -> set<int>
//   "{string:int}"                          -> map<string,int>
//
// The object/canonical form still works — passing an object passes
// straight through to the legacy BPPinType builder.

#pragma once

#include "BlueprintReaderTypes.h"

#include <nlohmann/json.hpp>
#include <string>

namespace bpr::tools {

// Parse `value` (either an object or a string) into a BPPinType.
// Throws std::invalid_argument on malformed input, with a message
// describing both the bad input and the expected forms.
BPPinType ParseTypeArg(const nlohmann::json& value);

// Just the string-parsing path, exposed for tests + reuse.
BPPinType ParseTypeShorthand(std::string_view shorthand);

} // namespace bpr::tools
