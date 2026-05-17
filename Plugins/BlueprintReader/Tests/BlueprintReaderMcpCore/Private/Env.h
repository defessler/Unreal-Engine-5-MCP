// Shared env-var + auto-discovery helpers. Used by BackendFactory (which
// reads server config from env) and the doctor/config subcommands (which
// also need to know what's configured + what's auto-detectable).
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bpr::env {

// Read an env var. Returns fallback when the var is unset or empty.
std::string GetOrDefault(const char* key, std::string fallback = "");

// Same, but returns std::nullopt when unset/empty. Distinguishes "user set
// the var" from "took the default" (useful for the validate-and-explain
// path).
std::optional<std::string> Get(const char* key);

// Parse a 0/1/true/false/yes/no/on/off env var. Unknown values produce a
// warning on `log` and return the fallback.
bool BoolOrDefault(const char* key, bool fallback, std::ostream& log);

// Read an int env var, falling back on parse error.
int IntOrDefault(const char* key, int fallback);

// ---------------------------------------------------------------------------
// Auto-discovery
// ---------------------------------------------------------------------------
//
// On the standard layout, the exe lives at:
//   <projectRoot>/Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe
//                        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
// Walking up 5 dirs gets us to <projectRoot>. From there we can find the
// .uproject and read its EngineAssociation, then resolve the engine path
// from the Windows registry (the same way UE itself does).

// Walk up from `start` looking for a single `.uproject` file. Returns its
// full path on success. Stops after `maxDepth` levels.
std::optional<std::filesystem::path>
FindUprojectAbove(const std::filesystem::path& start, int maxDepth = 6);

// Read `.uproject`'s EngineAssociation field.
std::optional<std::string>
ReadEngineAssociation(const std::filesystem::path& uproject);

// Resolve a Windows registry-style EngineAssociation
// (HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds\<key>) to the engine root
// path. Returns empty optional on Linux/Mac (where the registry doesn't
// apply), unset key, or missing path on disk.
std::optional<std::filesystem::path>
ResolveEngineFromRegistry(const std::string& engineAssociation);

// Looks at <plugin>/Binaries/Win64/UnrealEditor-BlueprintReaderEditor*.dll
// and returns the config name embedded in the suffix (or "Development" if
// the suffix-less DLL exists, or empty optional if none exist).
std::optional<std::string>
DetectEditorConfig(const std::filesystem::path& pluginDir);

}    // namespace bpr::env
