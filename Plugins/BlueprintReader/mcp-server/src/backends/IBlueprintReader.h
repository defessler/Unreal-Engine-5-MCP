// IBlueprintReader — the inner contract between the MCP tool layer and
// whatever is actually reading blueprint data (mock fixtures, commandlet
// subprocess, live-editor socket). All backends return the same canonical
// shapes from BlueprintReaderTypes.h.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "BlueprintReaderTypes.h"

namespace bpr::backends {

// Backend-side error type. The MCP layer catches this (and any std::exception)
// and surfaces it as an MCP tool error envelope.
class BlueprintReaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown when an asset path doesn't resolve. Sub-class so callers can
// distinguish "you typed the path wrong" from "I couldn't talk to the engine".
class AssetNotFound : public BlueprintReaderError {
public:
    using BlueprintReaderError::BlueprintReaderError;
};

class IBlueprintReader {
public:
    virtual ~IBlueprintReader() = default;

    virtual std::vector<BPAssetSummary> ListBlueprints(std::string_view path) = 0;
    virtual BPMetadata                  ReadBlueprint(std::string_view assetPath) = 0;
    virtual BPGraph                     GetGraph(std::string_view assetPath, std::string_view graphName) = 0;
    virtual BPFunction                  GetFunction(std::string_view assetPath, std::string_view fnName) = 0;
    virtual std::vector<BPVariable>     ListVariables(std::string_view assetPath) = 0;
    virtual std::vector<BPComponent>    GetComponents(std::string_view assetPath) = 0;
    // `kind`, when non-empty, additionally filters matches by their K2 extras
    // "kind" entry (e.g. "CallFunction", "VariableGet", "Event"). The text
    // `query` matches case-insensitively against class or title; `kind` is an
    // exact match (case-insensitive) against meta["kind"].
    virtual std::vector<BPNode>         FindNode(std::string_view assetPath, std::string_view query,
                                                 std::string_view kind = {}) = 0;

    // Write tools. Backends that don't support mutation throw
    // BlueprintReaderError. Each call should leave the .uasset compilable.

    // Add a member variable to a blueprint. `type` is the wire BPPinType.
    // `defaultValue`, `category` may be empty.
    virtual void AddVariable(std::string_view assetPath, std::string_view name,
                             const BPPinType& type, std::string_view defaultValue,
                             std::string_view category, bool replicated, bool editable) = 0;

    // Reposition a node by its GUID inside `graphName`.
    virtual void SetNodePosition(std::string_view assetPath, std::string_view graphName,
                                 std::string_view nodeId, int x, int y) = 0;

    // Delete a node by its GUID. Breaks any incoming/outgoing links first.
    virtual void DeleteNode(std::string_view assetPath, std::string_view graphName,
                            std::string_view nodeId) = 0;

    // Spawn a new node in `graphName` at (x, y). `kind` is one of:
    //   "Branch", "Sequence", "VariableGet", "VariableSet", "CallFunction",
    //   "CustomEvent". Kind-specific extras passed through the `extras` map
    //   (e.g. "Variable", "Function", "FunctionOwner", "EventName").
    // Returns the new node's GUID so the caller can wire pins to it.
    virtual std::string AddNode(std::string_view assetPath, std::string_view graphName,
                                std::string_view kind, int x, int y,
                                const std::map<std::string, std::string, std::less<>>& extras) = 0;

    // Connect two pins by node GUID + pin spec (GUID or name).
    virtual void WirePins(std::string_view assetPath, std::string_view graphName,
                          std::string_view fromNodeId, std::string_view fromPinSpec,
                          std::string_view toNodeId,   std::string_view toPinSpec) = 0;

    // Remove a member variable. Throws AssetNotFound if missing.
    virtual void DeleteVariable(std::string_view assetPath, std::string_view name) = 0;

    // Rename a member variable. Updates references in graphs.
    virtual void RenameVariable(std::string_view assetPath, std::string_view oldName,
                                std::string_view newName) = 0;

    // Add a new BP function graph. Returns the function name (echoed back)
    // plus the FunctionEntry node's GUID so callers can wire its `then`
    // exec output into their first statement without a follow-up read.
    struct AddFunctionResult {
        std::string functionName;
        std::string entryNodeId;  // empty if the plugin couldn't locate it
    };
    virtual AddFunctionResult AddFunction(std::string_view assetPath, std::string_view name) = 0;

    // Add an input parameter to an existing function. `type` is a wire BPPinType.
    virtual void AddFunctionInput(std::string_view assetPath, std::string_view functionName,
                                  std::string_view paramName, const BPPinType& type) = 0;

    // Add an output parameter to an existing function. Spawns a FunctionResult
    // node if there isn't one yet.
    virtual void AddFunctionOutput(std::string_view assetPath, std::string_view functionName,
                                   std::string_view paramName, const BPPinType& type) = 0;

    // Delete a function and its graph.
    virtual void DeleteFunction(std::string_view assetPath, std::string_view name) = 0;

    // Change a variable's default value (string form, as displayed in the Details panel).
    virtual void SetVariableDefault(std::string_view assetPath, std::string_view name,
                                    std::string_view newDefault) = 0;

    // Create a brand-new BP under `assetPath` (must be `/Game/...`) extending
    // `parentClass` (UClass path or short name). Idempotent — if the asset
    // already exists, returns without throwing. Required so AI agents can
    // generate whole new BPs, not just mutate existing ones (A3).
    struct CreateBlueprintResult {
        bool alreadyExisted = false;
        std::string parentClass;  // resolved full path, for echo
    };
    virtual CreateBlueprintResult CreateBlueprint(std::string_view assetPath,
                                                  std::string_view parentClass) = 0;

    // Set the literal default value on a node's pin (B1). Used by
    // compile_function's {lit:value} support — UE has no first-class
    // literal node, so the value is materialized as the consumer pin's
    // default. `pinSpec` accepts a pin GUID or a pin name.
    virtual void SetPinDefault(std::string_view assetPath,
                               std::string_view graphName,
                               std::string_view nodeId,
                               std::string_view pinSpec,
                               std::string_view value) = 0;

    // Change a member variable's type WITHOUT delete + re-add — UE
    // rewires every VariableGet / VariableSet node that references it
    // in place, so existing graphs survive (BP-2). For a brand-new
    // variable, use AddVariable instead.
    virtual void RetypeVariable(std::string_view assetPath,
                                std::string_view name,
                                const BPPinType& newType) = 0;

    // Change the My-Blueprint-panel category label on a member
    // variable. Empty `category` clears the label back to default
    // (BP-7).
    virtual void SetVariableCategory(std::string_view assetPath,
                                     std::string_view name,
                                     std::string_view category) = 0;

    // File-level duplicate of a blueprint (BP-5). `destAssetPath` must
    // be under /Game/. Idempotent — if the destination already exists,
    // returns alreadyExisted=true without overwriting.
    struct DuplicateBlueprintResult {
        bool alreadyExisted = false;
        std::string sourceAssetPath;
    };
    virtual DuplicateBlueprintResult DuplicateBlueprint(
        std::string_view sourceAssetPath, std::string_view destAssetPath) = 0;

    // Write a transpiled source file (.h or .cpp) into the project's
    // Source/ tree. Used by transpile_blueprint to drop the generated
    // UCLASS pair onto disk so UBT can compile it. The plugin validates
    // `destPath` is under <ProjectDir>/Source/ — no path-traversal escape.
    struct WriteGeneratedSourceResult {
        std::size_t bytesWritten = 0;
        std::string path;            // canonicalized absolute path
    };
    virtual WriteGeneratedSourceResult WriteGeneratedSource(
        std::string_view destPath, std::string_view content,
        bool createDirs = true) = 0;

    // ----- Project + Content Browser ops ---------------------------------
    //
    // Project-level introspection + asset-browser operations that complement
    // the per-Blueprint surface. These are surface-level ops users expect
    // when working with a UE project as a whole.

    // Read the project's metadata (`.uproject` JSON + a normalized view of
    // the most-asked-for fields). Backends override; default throws.
    struct ProjectMetadata {
        std::string projectName;        // derived from .uproject filename
        std::string projectPath;        // absolute path to the .uproject
        std::string engineAssociation;  // .uproject "EngineAssociation"
        std::string category;
        std::string description;
        nlohmann::json raw;             // full .uproject JSON for anything else
    };
    virtual ProjectMetadata GetProjectMetadata() {
        throw BlueprintReaderError("GetProjectMetadata not supported by this backend");
    }

    // Save every dirty package the editor has loaded. With `dirtyOnly=true`
    // (default), packages that aren't marked dirty are skipped — fast no-op
    // when nothing's changed. Live backend hits the editor's save path;
    // commandlet daemon walks loaded packages.
    struct SaveAllResult {
        int savedCount = 0;
        std::vector<std::string> failedAssets;
    };
    virtual SaveAllResult SaveAll(bool dirtyOnly = true) {
        (void)dirtyOnly;
        throw BlueprintReaderError("SaveAll not supported by this backend");
    }

    // Move (or rename) an asset. `dest` is the full destination package
    // path — pass the same folder with a different leaf for a rename, or
    // a different folder to move. Both must be under `/Game/`. Updates
    // the asset registry, fixes up references in other assets.
    struct MoveAssetResult {
        std::string sourcePath;
        std::string destPath;
        int redirectorsCreated = 0;
    };
    virtual MoveAssetResult MoveAsset(std::string_view sourcePath,
                                      std::string_view destPath) {
        (void)sourcePath; (void)destPath;
        throw BlueprintReaderError("MoveAsset not supported by this backend");
    }

    // Delete an asset. Refuses if other assets reference it (default;
    // override with `force=true`). Returns the list of references found so
    // the caller can act on them — fix-up or force.
    struct DeleteAssetResult {
        std::string path;
        bool deleted = false;
        std::vector<std::string> referencingAssets;
    };
    virtual DeleteAssetResult DeleteAsset(std::string_view assetPath,
                                          bool force = false) {
        (void)assetPath; (void)force;
        throw BlueprintReaderError("DeleteAsset not supported by this backend");
    }

    // Create a folder under `/Game/`. Idempotent — returns
    // {already_existed:true} if the folder is already present. UE folders
    // are just package paths with a stub asset; we use
    // `IAssetTools::CreateUniqueAssetName` semantics indirectly.
    struct CreateFolderResult {
        std::string path;
        bool alreadyExisted = false;
    };
    virtual CreateFolderResult CreateFolder(std::string_view folderPath) {
        (void)folderPath;
        throw BlueprintReaderError("CreateFolder not supported by this backend");
    }

    // List all UDataTable assets under a content path (mirrors
    // ListBlueprints but for the DataTable type).
    virtual std::vector<BPAssetSummary> ListDataTables(std::string_view path) {
        (void)path;
        throw BlueprintReaderError("ListDataTables not supported by this backend");
    }

    // Read a single DataTable's row structure + every row's field values.
    struct DataTableInfo {
        std::string assetPath;
        std::string rowStruct;          // e.g. "/Game/Data/ST_Item"
        std::vector<std::string> columns;
        // Each row: { "row_name": <FName>, "<col>": <serialized-string>, ... }
        nlohmann::json rows = nlohmann::json::array();
    };
    virtual DataTableInfo ReadDataTable(std::string_view assetPath) {
        (void)assetPath;
        throw BlueprintReaderError("ReadDataTable not supported by this backend");
    }

    // ----- Live editor ops -----------------------------------------------
    //
    // These are most useful with an open editor (live backend). The
    // commandlet daemon still routes them — they operate on the headless
    // editor's state — but PIE start/stop in commandlet mode is weird
    // semantically. Auto backend prefers live whenever the editor is open.

    // Execute a UE console command (e.g. "stat unit", "showflag.bones 1").
    // Returns whatever the command echoed to the log + an `ok` flag.
    struct ConsoleCommandResult {
        std::string output;   // log buffer captured during execution
    };
    virtual ConsoleCommandResult ConsoleCommand(std::string_view command) {
        (void)command;
        throw BlueprintReaderError("ConsoleCommand not supported by this backend");
    }

    // Read / write a console variable (CVar). Get returns the current
    // value as a string; Set forces ECVF_SetByCode priority.
    struct CVarValue {
        std::string name;
        std::string value;
        std::string help;     // CVar's help text, if registered
        bool        exists = false;
    };
    virtual CVarValue GetCVar(std::string_view name) {
        (void)name;
        throw BlueprintReaderError("GetCVar not supported by this backend");
    }
    virtual CVarValue SetCVar(std::string_view name, std::string_view value) {
        (void)name; (void)value;
        throw BlueprintReaderError("SetCVar not supported by this backend");
    }

    // Start / stop Play-In-Editor. PIE modes: "selected_viewport" (default),
    // "new_editor_window", "standalone", "vr_preview". Stop is a no-op when
    // PIE isn't active.
    struct PieResult {
        bool started = false;
        bool stopped = false;
        std::string mode;
    };
    virtual PieResult PieStart(std::string_view mode = "selected_viewport") {
        (void)mode;
        throw BlueprintReaderError("PieStart not supported by this backend");
    }
    virtual PieResult PieStop() {
        throw BlueprintReaderError("PieStop not supported by this backend");
    }

    // Trigger a Live Coding compile + patch. Returns whether the compile
    // was queued; the actual result is asynchronous (Live Coding emits
    // its own status messages to the log).
    struct LiveCodingResult {
        bool queued = false;
        std::string message;
    };
    virtual LiveCodingResult LiveCodingCompile() {
        throw BlueprintReaderError("LiveCodingCompile not supported by this backend");
    }

    // Editor selection — names of currently-selected actors in the level.
    struct SelectionResult {
        std::vector<std::string> actorNames;
    };
    virtual SelectionResult GetSelectedActors() {
        throw BlueprintReaderError("GetSelectedActors not supported by this backend");
    }
    // `replace=true` clears existing selection first; `false` adds to it.
    virtual SelectionResult SetSelection(
        const std::vector<std::string>& actorNames, bool replace = true) {
        (void)actorNames; (void)replace;
        throw BlueprintReaderError("SetSelection not supported by this backend");
    }

    // Spawn an actor in the current level. `class_path` is the full UClass
    // path (e.g. "/Script/Engine.StaticMeshActor" or a BP class). Location
    // / rotation / scale default to zero / identity / one if unspecified.
    struct SpawnActorResult {
        std::string actorName;   // the spawned actor's GetName()
        std::string actorLabel;  // the user-facing label in the outliner
    };
    virtual SpawnActorResult SpawnActor(
        std::string_view classPath,
        double locX = 0, double locY = 0, double locZ = 0,
        double rotPitch = 0, double rotYaw = 0, double rotRoll = 0,
        double scaleX = 1, double scaleY = 1, double scaleZ = 1) {
        (void)classPath;
        (void)locX; (void)locY; (void)locZ;
        (void)rotPitch; (void)rotYaw; (void)rotRoll;
        (void)scaleX; (void)scaleY; (void)scaleZ;
        throw BlueprintReaderError("SpawnActor not supported by this backend");
    }

    // Update an existing actor's transform. Identified by name (the
    // string returned by SpawnActor / GetSelectedActors).
    virtual void SetActorTransform(
        std::string_view actorName,
        double locX, double locY, double locZ,
        double rotPitch, double rotYaw, double rotRoll,
        double scaleX, double scaleY, double scaleZ) {
        (void)actorName;
        (void)locX; (void)locY; (void)locZ;
        (void)rotPitch; (void)rotYaw; (void)rotRoll;
        (void)scaleX; (void)scaleY; (void)scaleZ;
        throw BlueprintReaderError("SetActorTransform not supported by this backend");
    }

    // Destroy an actor by name.
    struct DeleteActorResult {
        bool deleted = false;
    };
    virtual DeleteActorResult DeleteActor(std::string_view actorName) {
        (void)actorName;
        throw BlueprintReaderError("DeleteActor not supported by this backend");
    }

    // Read the recent output-log buffer. The plugin module installs a
    // ring-buffer output-device sink at startup; this returns up to
    // `limit` of the most recent entries, optionally filtered by minimum
    // severity ("Display" / "Log" / "Warning" / "Error" / "Fatal").
    struct LogEntry {
        std::string severity;   // verbosity name
        std::string category;
        std::string message;
        std::string timestamp;  // ISO-8601 (if captured)
    };
    struct OutputLogResult {
        std::vector<LogEntry> entries;
    };
    virtual OutputLogResult ReadOutputLog(int limit = 200,
                                          std::string_view minSeverity = {}) {
        (void)limit; (void)minSeverity;
        throw BlueprintReaderError("ReadOutputLog not supported by this backend");
    }

    // ----- Automation tests ---------------------------------------------
    //
    // Trigger UE's automation test framework. `pattern` is the test-name
    // wildcard (e.g. "BlueprintReader.*", "*Smoke*"); empty means "every
    // registered test". Results land in the output log; this tool kicks
    // off the run and returns immediately.
    struct AutomationRunResult {
        bool started = false;
        std::string message;
    };
    virtual AutomationRunResult RunAutomationTests(std::string_view pattern) {
        (void)pattern;
        throw BlueprintReaderError("RunAutomationTests not supported by this backend");
    }

    // ----- Batch sentinels (A1) ------------------------------------------------
    // BeginBatch / EndBatch wrap a sequence of write ops so the expensive
    // CompileBlueprint + SavePackage runs once per affected BP at EndBatch
    // instead of once per op. apply_ops uses this to collapse N×compile to 1.
    //
    // Default no-op so backends that don't care (mock, future read-only) need
    // no changes. CommandletBlueprintReader overrides to emit the matching
    // -Op=BeginBatch / -Op=EndBatch lines to the daemon.
    //
    // Best-effort failure semantics: if a batch is open and a write op throws,
    // EndBatch should still be called by the caller (in a try/finally pattern)
    // and will compile+save whatever ops landed before the failure.
    //
    // EndBatch returns a JSON object describing the flush: `{ok, recompiled,
    // diagnostics, error_count, warning_count}` (C1). Default implementation
    // returns an empty object — backends without a real compile step have
    // nothing to surface.
    //
    // `skipCompile` is the on_failure="skip" path — the caller knows
    // something failed mid-batch and doesn't want partial state on disk.
    // Plugin honors this by skipping the per-BP compile + save loop in
    // EndBatch (in-memory state stays dirty until daemon restarts;
    // documented as a limitation of strict-atomic mode).
    virtual void BeginBatch() {}
    virtual nlohmann::json EndBatch(bool /*skipCompile*/ = false) {
        return nlohmann::json::object();
    }

    // Tear down any backing process / connection / cache the backend holds
    // open. Optional — default is a no-op for backends that don't have one
    // (mock, future live). The CommandletBlueprintReader override sends
    // QUIT to its daemon and joins, freeing the project lock so the user
    // can launch the full editor (or another tool) without contention.
    //
    // Subsequent tool calls auto-respawn the daemon — same path the
    // existing daemon-died fallback uses. So this is safe to call ad-hoc;
    // the next read just pays a one-time cold start.
    //
    // Returns a JSON object describing what happened: {ok:true,
    // was_running:bool, ...}. Backends without a teardownable resource
    // return {ok:true, was_running:false}.
    virtual nlohmann::json ShutdownDaemon() {
        return nlohmann::json{{"ok", true}, {"was_running", false}};
    }
};

} // namespace bpr::backends
