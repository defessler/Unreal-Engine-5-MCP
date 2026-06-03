// Server-side response projection — apply caller-specified field
// selection to a JSON response, drop everything else.
//
// Why: the AI client (Claude / Copilot / etc.) treats every byte of a
// tool response as context tokens it has to consume. read_blueprint on
// a busy BP returns variable lists, graph summaries, function lists,
// macros, interfaces — hundreds of fields. If the caller only needs
// "what's the parent class?" they shouldn't pay tokens for the rest.
//
// Field selection is a list of dotted paths. Top-level keys not on the
// list are dropped. For arrays, we apply the same filter to each
// element (so `variables[].name` keeps just `name` on every variable).
//
// Examples:
//   ["parent_class"]                       -> { parent_class }
//   ["name", "variables[].name"]           -> { name, variables: [{name}, ...] }
//   ["functions[].name", "graphs[].name"]  -> just the names of each
//
// `[]` is a literal segment meaning "apply the rest to every element of
// this array". A path with no `[]` selects an object key. Paths that
// don't match anything in the doc are silently ignored.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools {

// Apply field projection to `body` in place. Empty paths -> no-op
// (back-compat: tools without explicit `fields` arg get full payloads).
void ApplyProjection(nlohmann::json& body, const std::vector<std::string>& paths);

// Pull a `fields` array argument from a tool's args; returns empty if
// absent / null. Throws on non-array, non-string-element values.
std::vector<std::string> ParseFieldsArg(const nlohmann::json& args);

// Given the ORIGINAL (pre-projection) body and the requested field paths,
// return a human-readable warning for each top-level field name that matched
// NO response key — almost always a typo (e.g. `asset_paths` for
// `asset_path`). Each warning lists the available keys as a hint. Returns
// empty when every requested field matched, or when the body has no keys to
// match against (a not-found / empty payload — can't tell). The caller
// attaches these under a `_warnings` key so a `fields` typo can't silently
// project nothing and lead the agent to a wrong conclusion. Doc-aware: uses
// the same alias/`is_`-convention matching as ApplyProjection.
std::vector<std::string> FieldsProjectionWarnings(
	const nlohmann::json& body, const std::vector<std::string>& paths);

}    // namespace bpr::tools
