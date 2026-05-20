// Canonical hints (per MCP 2025-03-26 §tools/annotations) for every
// tool this server ships. Clients use these to filter their UI
// (Copilot's "read-only tools only" toggle, Claude Code's allowlist
// rules, etc.) without having to grok 100+ tool descriptions.
//
// `AnnotationsFor(name)` is consulted automatically from
// ToolRegistry::Add when the descriptor's `annotations` field is
// still in its default (all-nullopt) state — so the existing
// registration sites in BlueprintTools.cpp / ApplyOps.cpp /
// CompileFunction.cpp / ToolsetMeta.cpp don't need touching.
//
// Adding a new tool: add its name to the appropriate set in
// ToolAnnotations.cpp. Read-only (introspection / lookup) tools
// go in `ReadOnlySet()`; everything else in `WriteSet()` plus
// any applicable override (`DestructiveSet`, `OpenWorldSet`,
// `IdempotentSet`).
//
// Spec note: omitting an annotations field leaves the client to
// assume the spec defaults — readOnlyHint=false, destructiveHint=true,
// idempotentHint=false, openWorldHint=true. Because *destructive*
// and *openWorld* default to true, we EXPLICITLY set =false on
// every recognized tool that's neither destructive nor open-world.
// That way a Copilot user filtering "only non-destructive" doesn't
// see every additive write tool flagged.

#pragma once

#include "tools/ToolRegistry.h"

#include <string>

namespace bpr::tools {

// Look up the canonical annotation hints for a tool by name. Returns
// an empty (all-nullopt) ToolAnnotations for names not recognized by
// this server's tool set — the caller (ToolRegistry::Add) then leaves
// the descriptor's annotations untouched.
ToolAnnotations AnnotationsFor(const std::string& tool_name);

}    // namespace bpr::tools
