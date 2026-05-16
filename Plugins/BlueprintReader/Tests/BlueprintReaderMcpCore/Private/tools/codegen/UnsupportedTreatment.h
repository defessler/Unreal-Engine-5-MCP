// UnsupportedTreatment — best-effort handling for BP nodes that don't
// have a clean compilable-C++ equivalent.
//
// The decompile pass emits `{unsupported: {node_class, guid, reason,
// fields}}` for any K2 node it can't structurally represent (timelines,
// latent actions, anim graph nodes, etc.). This module classifies them:
//
//   - Recognized "approximation" patterns get a best-effort C++ stub
//     instead of just a TODO. E.g. K2Node_SpawnActorFromClass becomes a
//     `GetWorld()->SpawnActor<>()` call with a comment listing what
//     the agent still needs to verify.
//
//   - Every encountered unsupported node — whether stub or pure TODO —
//     adds an entry to a structured sidecar JSON the caller can write
//     next to the generated .cpp. The agent uses this to iterate over
//     "what manual work remains" without re-parsing the source.
//
// Inspiration: Hazelight's UnrealEngine-Angelscript repo handles a
// similar problem (BP-incompatible UE constructs that need bespoke
// translation rules). We borrow their pattern of a class→treatment
// table; entries grow as users hit real cases.
//
// Sidecar JSON shape:
//   {
//     "version": 1,
//     "generated_at": "<ISO 8601>",
//     "source_bp": "/Game/AI/BP_Enemy",
//     "generated_files": ["BP_Enemy_Generated.h", "BP_Enemy_Generated.cpp"],
//     "unsupported_nodes": [
//       { "guid": "...", "class": "K2Node_Timeline", "function": "TakeDamage",
//         "treatment": "todo_comment",
//         "manual_steps": ["Configure 'Fade' curve asset"] }
//     ],
//     "approximations": [
//       { "guid": "...", "class": "K2Node_SpawnActorFromClass", "function": "TakeDamage",
//         "treatment": "spawn_actor_call",
//         "verify": "SpawnParameters' bNoCollisionFail equivalent (deprecated in 5.x)" }
//     ]
//   }

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace bpr::tools {

// Decision returned by ClassifyUnsupported. Drives both the inline
// rendering and the sidecar entry shape.
struct UnsupportedClassification {
	enum class Kind {
		TodoComment,    // no good substitution; emit `// TODO[bpr-unsupported]`
		Approximation,  // best-effort C++ stub generated; user verifies
	};
	Kind kind = Kind::TodoComment;

	// Pre-built C++ snippet for inline emission (when Kind ==
	// Approximation). Empty for TodoComment kind.
	std::string snippet;

	// Human-readable summary that goes into the sidecar's
	// manual_steps / verify fields.
	std::string note;
};

// Inspect an `{unsupported: {node_class, ...}}` BPIR statement and
// return the appropriate treatment. Pure function — caller writes
// snippets / sidecars based on the result.
UnsupportedClassification ClassifyUnsupported(const nlohmann::json& unsupportedField);

// Build a sidecar JSON document from a list of CppEmit-format notes
// (statement form: each entry is the `unsupported` object plus a
// `treatment` field added by CppEmit). Adds metadata about the source
// BP + generated files. Returns a top-level object suitable for
// writing to disk as `<class>.transpile-notes.json`.
nlohmann::json BuildSidecar(std::string_view sourceBp,
							const std::vector<std::string>& generatedFiles,
							const nlohmann::json& notes);

} // namespace bpr::tools
