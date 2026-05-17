# Scripting languages — AngelScript, Lua (UnLua), Verse

Research note for the bp-reader MCP server roadmap. Audience: someone
considering adding a non-C++ scripting backend so the server can
decompile BPs into, and compile from, a language other than C++.

The current pipeline pivots through BPIR
(`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.h`):

```
BP graph  --DecompileFunction-->  BPIR  --EmitCppFunction-->   C++
C++       --ParseCppFunction--->  BPIR  --compile_function-->  BP graph
```

The codegen and parse seams that any new backend slots into are
`tools/codegen/CppEmit.h` and `tools/parse/CppParse.h`. Both are
documented with the constraints a frontend / backend must satisfy
(throw on unknown forms, validate against `ValidateBpir`, round-trip
identity on the patterns the matching pair produces).

This note answers: which scripting languages plausibly hook in here,
and what shape would a Lua / AngelScript / Verse backend take.

**Network access constraint.** Both `WebFetch` and `WebSearch` were
denied in the environment where this note was written. The
descriptions of AngelScript, UnLua, and Verse below draw on prior
knowledge of the projects; every concrete claim that could not be
re-verified against the live repos / docs is flagged "Not verified"
inline. A follow-up pass with web access should walk these and
either confirm or correct.

---

## 1. AngelScript (Hazelight fork)

**Repo.** `https://github.com/Hazelight/UnrealEngine-Angelscript` — a
fork of mainline Unreal Engine maintained by Hazelight (the studio
behind *It Takes Two* and *Split Fiction*), with first-class
AngelScript integration grafted onto the engine. It is **not** a
plugin you drop into a stock engine — it ships as a patched engine
fork. Users must build from the fork to use it. ("Not verified" that
the current head no longer offers a plugin form — historically a
plugin variant has not been part of the public distribution.)

**Source tree.** The bulk of the integration lives under
`Engine/Plugins/Angelscript/Source/` ("Not verified" — verify exact
path against the current fork head; older commits used
`Engine/Source/Programs/Angelscript/` for parts of the toolchain).
The plugin pulls in the upstream `angelscript` library (`asIScriptEngine`,
`asIScriptModule`, `asIScriptContext`) and wraps it with UE-aware
glue: type registration, `UClass` synthesis, and the hot-reload
pipeline.

**Editor hook surface.** AngelScript participates at three stages:

1. **Engine init** — registers AS-native types for every reflected
   UE type the user has marked AS-visible. The bindings are generated
   from UHT metadata, so any `UCLASS`, `UFUNCTION`, `UPROPERTY`,
   `USTRUCT`, or `UENUM` declared in C++ is callable from
   AngelScript with no per-type bridge code. The reverse — AS-defined
   classes — is wired by synthesizing a `UClass` at runtime.

2. **Custom UClass synthesis at startup.** When the AS module scans
   `Script/` (or whichever content-relative directory holds `.as`
   files), it parses each file, then for every AS class deriving from
   a `UCLASS` it builds a corresponding UE `UClass` object on the fly.
   The `UClass` gets the AS class's reflected fields registered as
   `FProperty`s and its methods as `UFunction`s with bytecode that
   thunks into the AS context. ("Not verified" — verify whether
   AS classes appear in the Content Browser as first-class assets
   or only as code-visible classes; historically they have shown up
   in pickers like native UCLASSes.)

3. **Hot-reload pipeline.** AS source is re-parsed on edit; classes
   are re-synthesized; live `UObject` instances of the affected
   `UClass` are re-instanced to pick up the new shape. This is the
   feature that makes AS attractive as a gameplay-iteration language
   — sub-second edit-test-loop without needing Live Coding or a
   restart. The reinstancer reuses UE's existing BP-reinstance
   machinery ("Not verified" — verify whether it uses
   `FBlueprintCompileReinstancer` or a parallel codepath specific to
   AS modules).

**UFUNCTION registration.** Two directions:

- **C++ → AS:** every `UFUNCTION(BlueprintCallable)` is also AS-callable
  through the auto-generated bindings. No extra annotation needed —
  if Blueprint can call it, AS can call it. ("Not verified" —
  verify whether AS gates on the `BlueprintCallable` specifier or
  on `BlueprintType` / a separate `AngelscriptCallable` specifier.)

- **AS → BP:** AS class methods get `UFunction` objects registered
  on the synthesized `UClass`. Whether they appear in BP pickers
  depends on AS-side decorators ("Not verified" — current fork
  syntax is something like `UFUNCTION(BlueprintCallable)` written
  inline in `.as`, mirroring C++; verify against repo).

**Syntax compared to Blueprint.** AS is the closest of the three to BP
in semantic distance:

- Statically typed (BP pins are statically typed).
- Strongly typed UE bindings — `AActor*`, `FVector`, `UMyComponent*`
  appear with the engine's type names.
- Imperative control flow (`if`, `for`, `while`) — exactly what BP
  exec wires already encode.
- No traits / generics / lambdas of any sophistication (so it stays
  within reach of BPIR statement / expression forms; nothing in the
  AS surface area lacks a BPIR home).
- Manual memory: `UObject`-rooted, GC-traced via UE's GC; raw
  AngelScript objects are reference-counted by the AS runtime.

A function written in AS looks essentially like the same function
written in C++ minus headers / macros:

```as
UFUNCTION(BlueprintCallable)
bool TakeDamage(float Damage)
{
    if (bIsAlive)
    {
        Health -= Damage;
        if (Health <= 0.0)
        {
            bIsAlive = false;
        }
    }
    return bIsAlive;
}
```

**Concrete bp-reader sketch — "if we added an AS backend".**

What "AS backend" means in this server: a frontend (AS → BPIR) plus a
backend (BPIR → AS), mirroring the C++ pair. Adding it would not
require touching `IBlueprintReader` — the AS frontend / backend is
pure CPU work against BPIR, just like CppEmit / CppParse. The
asset-side write tools (`compile_function`, `apply_ops`, etc.) stay
on the existing K2-node-based path.

New files (mirror of the C++ pair):

```
tools/codegen/AsEmit.h             // BPIR → AngelScript source
tools/codegen/AsEmit.cpp
tools/codegen/AsClassEmit.h        // full AS class (mirrors CppClassEmit)
tools/codegen/AsClassEmit.cpp
tools/parse/AsLex.h                // tokenizer
tools/parse/AsLex.cpp
tools/parse/AsParse.h              // AS source → BPIR
tools/parse/AsParse.cpp
```

New MCP tools (mirror `decompile_function` / `transpile_function`):

- `decompile_function_as` — BP → BPIR → AS string. Or extend the
  existing tool with a `target_language` arg.
- `transpile_function_as` — AS source → BPIR → BP. Same pattern.
- `parse_as_function` — AS source → BPIR, no BP write (useful for
  CI / lint / dry-run).
- `write_generated_source_as` — analog of `write_generated_source`,
  writes the AS file into the project's `Script/` dir.

`ReadAngelscriptFunction` shape — what would such a method look like
on `IBlueprintReader`? Trick question. AS files live on disk as
`.as`, not as `.uasset` — there's no UClass to introspect through
UHT-style reflection at the source level (the UClass is *synthesized*
from the AS source at module load). So `ReadAngelscriptFunction`
would either:

- (a) **Read the .as text directly off disk.** `IBlueprintReader`
  doesn't deal with raw files today; this would either be a new
  interface method (`virtual std::string ReadAngelscriptSource(...)
  = 0;`) or pulled out of the interface entirely into a sibling
  `IScriptBackend` (see §5).
- (b) **Read the synthesized UClass at runtime via the
  AssetRegistry**, which works only when the AS module has loaded
  the class. Less useful for a server that wants the source-of-truth
  representation (AS text), more useful for "show me the runtime
  shape of this AS class".

The (a) approach mirrors how UE's own AS plugin treats the source
files as the canonical artifact. The (b) approach gives the server
the same view it has of regular BPs.

A `transpile_function` → AS pass would be:

1. Decompile the BP function to BPIR (existing path; no AS-specific
   work).
2. Run the new `EmitAsFunction(bpirFunctionDoc)` pass — same shape
   as `EmitCppFunction` but with AS syntax and AS type names. Type
   mapping is simpler than C++ (no `TObjectPtr`, no
   `const FString&` aesthetics) — `MapBpirTypeToAs` is shorter than
   the three C++ mappers combined.
3. Write the file into the project's AS source directory.
4. Hot-reload picks it up automatically (no commandlet step). This
   is one of the appealing properties of an AS target compared to a
   C++ target — no Build.bat, no Live Coding, no editor restart.

**Caveats.**

- The AS fork is a separate engine maintenance burden. Most projects
  using AS pin to a specific UE major version that the fork has caught
  up to. bp-reader targeting AS would only work for projects already
  on the fork (or on a fork derivative).
- AS does not ship in mainline UE. "Not verified" whether Epic plans
  to absorb it (some signals around 2024-2025 suggested Epic was at
  least aware; nothing official as of the cutoff).
- The synthesized-UClass approach means AS-defined types are
  invisible to systems that scan the asset registry for `.uasset`s
  but visible to systems that scan loaded `UClass`s. bp-reader's
  `list` tool would need to be aware of this if it surfaces AS
  classes alongside BPs.

---

## 2. UnLua (sol2-class Lua bridge)

**Repo.** `https://github.com/Tencent/UnLua` — open-source Lua
integration maintained by Tencent. Production-leaning (used in
PUBG Mobile and other Tencent titles, "Not verified" against
current README claims). Drops in as a UE plugin, no engine fork
required. Targets stock UE.

**Architecture.** The defining design choice is that UnLua does
**not** define new `UClass`es from Lua. Instead, it lets a Lua file
**override** an existing `UClass` (BP class or C++ class) by hooking
the dispatch of UE's reflection system. Lua provides the implementation
of UFUNCTIONs that the host BP/C++ class declares.

**The ProcessEvent patch.** UnLua patches `UObject::ProcessEvent` —
the central reflection-call dispatch entry point — so that before
the engine runs a UFUNCTION's bytecode / native thunk, UnLua checks
whether the calling instance has a Lua override registered for that
function. If yes, control transfers to the Lua handler, which can
read arguments, run, and write back outputs through the Lua-to-UE
property marshaling layer. If no, the original engine codepath runs.

The patch is installed at engine init: ("Not verified" — verify
whether it overrides `ProcessEvent` itself by patching the vtable,
patches it per-class, or hooks through a different reflection seam
like `FNativeFunctionRegistrar`. Historically UnLua has documented
the vtable-replacement approach; verify the current head matches.)

**The class-binding model.**

```lua
-- MyActor.lua
local MyActor = UnLua.Class()

function MyActor:ReceiveBeginPlay()
    print("Hello from Lua")
end

function MyActor:TakeDamage(Damage)
    self.Health = self.Health - Damage
    return self.bIsAlive
end

return MyActor
```

A Lua file is associated with a UClass through one of:

- (a) Adding an `UnLuaInterface` to the BP and returning the Lua
  module path.
- (b) Naming convention pinned in `UnLua.ini`.
- (c) Explicit registration call in C++ at module-load time.

("Not verified" — verify the canonical association method as of the
current README; historically (a) has been the most common.)

When `ReceiveBeginPlay` fires on an instance of that class, UnLua's
ProcessEvent hook routes the call into Lua, which executes
`MyActor:ReceiveBeginPlay`. From Lua's perspective, `self` is a
proxy table that lazily forwards property reads / writes and method
calls back through the UE reflection system.

**Lua source representation.** Lua source is parsed by Lua itself
(via `luaL_loadbuffer` / `luaL_loadstring`). UnLua does **not** ship
a Lua AST. The frontend is the standard Lua interpreter; the script
runs in a `lua_State` and interacts with UE through UnLua's binding
glue (analogous in spirit to sol2, though UnLua predates and is
independent of sol2 — "Not verified" against current architecture
docs).

This is the critical point for bp-reader: **there is no programmable
AST for Lua available from UnLua's surface.** A "BP → Lua"
decompile path through the bp-reader server has two options:

1. **Pretty-printer.** Take the BPIR, run a hand-rolled
   `EmitLuaFunction` pass that emits Lua source. No AST round-trip
   on the Lua side — we emit text, the user (or `luac`) is the only
   thing that ever parses it. Lossy: edits to the emitted `.lua` are
   never reconciled back into the BP. Equivalent to writing
   companion `_Generated.lua` files alongside a BP, where the BP
   stays the source of truth.

2. **Vendor a third-party Lua parser** to build "Lua → BPIR"
   ourselves. Candidates: LuaParser (small, header-only),
   tree-sitter-lua via the C bindings (heavier but well-supported),
   or hand-rolling. Once we have a Lua AST, we walk it the same way
   `CppParse` walks tokens and emit BPIR. Then Lua becomes a
   first-class round-trip language alongside C++. This is more work
   but is the only path where Lua source can serve as the source of
   truth.

**Sketch — `transpile_function` → Lua backend.**

Pretty-printer route only (the heavy option is a separate research
question):

1. New files:

   ```
   tools/codegen/LuaEmit.h             // BPIR → Lua source
   tools/codegen/LuaEmit.cpp
   tools/codegen/LuaClassEmit.h        // full Lua class module
   tools/codegen/LuaClassEmit.cpp
   ```

2. `EmitLuaFunction(bpirFunctionDoc)` walks the BPIR statement
   forms (`if`, `set`, `call`, `return`, `cast`, `switch`,
   `for_each`, `while`) and emits Lua control flow. The expression
   forms (`var`, `lit`, `call`, `cast`, `member`, `index`, `self`,
   `new_array`, `new_struct`) map cleanly to Lua syntax —
   `new_struct` becomes a Lua table constructor, `cast` becomes a
   no-op or a `getmetatable`-based check, `self` is `self`.

3. Type mapping is simpler than C++ — Lua is dynamic, so most of
   the BPIR type info is dropped at emit time. We can preserve it
   in EmitLua-Annotated mode as comments.

4. **No `compile_function` analog** — without a Lua parser, the
   reverse direction (Lua → BP) is not available. We get
   "BP → Lua" only.

5. UnLua-aware emission: if the user wants the emitted Lua to plug
   into UnLua, we wrap with `UnLua.Class()` and emit `self:Name(...)`
   instead of bare `function Name(...)`. Otherwise we emit plain
   Lua functions. Driven by an option in `LuaEmitOptions`.

**Runtime hook with bp-reader.** UnLua and bp-reader's commandlet
backend are independent — bp-reader's `set_breakpoint` /
`bp_reader.list` console commands wouldn't see Lua handlers because
they're not BP nodes. If we ever want bp-reader to surface
"Lua functions overriding this BP method", we need a runtime probe
that asks UnLua's registry for current bindings on a given UClass.
That is a separate tool (`list_lua_overrides`), not part of the
transpile pipeline.

---

## 3. Verse

**Confirm: UEFN-only as of UE5.7.** Verse ships as the scripting
language of Unreal Editor For Fortnite (UEFN), Epic's Fortnite Creative
authoring tool. Mainline Unreal Engine 5.7 does **not** ship Verse.
There has been longstanding Epic messaging that Verse will eventually
land in mainline UE; the current public roadmap pin is "with UE6" but
that has slipped several times. ("Not verified" — verify against
Epic's latest public statements; historically the date has moved
from 2023 → 2024 → 2025 → UE6.)

This makes Verse a non-target for any UE5.7-only bp-reader work. It
is described here for completeness because the user explicitly
asked about it.

**Language properties.**

- **Static.** Types are checked at compile time, no implicit
  coercions, generics with constraints.
- **Deterministic.** Verse is built around the "Verse Calculus" — a
  pure functional core with effect annotations. Side effects are
  explicit and ordered. Two executions of the same Verse program with
  the same inputs produce the same outputs.
- **Transactional.** The "transact" effect lets Verse roll back
  side effects if a branch fails. Combined with failable expressions
  (where `if x?` reads "x evaluates without failure"), this gives
  Verse a structured-failure model very different from C++ /
  Blueprint / AS.
- **Concurrency-first.** First-class structured concurrency via
  `spawn`, `race`, `sync`, and `coroutine`-shaped expressions.
- **Decides class.** Functions are tagged with effect classes
  (`computes`, `converges`, `decides`, `transacts`, `varies`,
  `reads`, `writes`, `allocates`, `suspends`) that the type checker
  enforces. (Several of these effect names are accurate; ordering
  and exact spelling: "Not verified".)

**Module structure.**

```
MyVerseProject/
├── Verse.toml          // package manifest (deps, package name)
└── src/
    ├── MyDevice.verse  // .verse source files
    └── HelperLib.verse
```

The `Verse.toml` carries the package identity, dependency
declarations (`fortnite.com`, `Verse.org`), and platform target.
("Not verified" — `Verse.toml` is the published packaging file
name; verify exact key names against the current UEFN docs.)

**Why it doesn't fit the bp-reader pattern.**

- **No C++ host introspection.** Verse types are not `UCLASS`es.
  Verse classes do not synthesize UClasses (as AS does) or override
  UClass dispatch (as UnLua does). The Verse runtime is its own VM
  (the Verse-Calculus interpreter) that sits beside UE's `UObject`
  system rather than plugging into it via reflection.
- **No UFUNCTION bridge.** Verse interoperates with the engine via
  a curated digest of UE types exposed as Verse interfaces in the
  `Verse.org` / `Fortnite.com` packages. Adding a new UE type to
  Verse is a Verse-VM-side operation, not a "decorate it with
  `VERSE_CALLABLE` and rebuild" operation.
- **No editor file representation bp-reader recognizes.** `.verse`
  is plain text on disk, parsed by the Verse compiler. There's no
  `.uasset` shape, no UClass, no `IBlueprintReader` method that
  would return anything meaningful.
- **No round-trip with K2 graphs is sensible.** Verse's
  effect / failure / concurrency model is strictly more expressive
  than the K2 graph model in some axes (transactions, structured
  concurrency, decides effects) and strictly less expressive in
  others (no event graphs, no Blueprint-style "expose for designer"
  with red exec wires). The two are not compatible IRs.

**What changes when/if Verse lands in mainline UE6.**

Speculative, but:

- If Verse ships with a UClass-synthesis story analogous to
  AngelScript's, then a Verse backend on bp-reader becomes plausible.
  We'd need a new IR alongside BPIR (call it VerseIR) because the
  effect-typed and failure-typed forms have no clean BPIR home, and
  we'd want to keep BPIR clean of Verse-specific shapes.
- More likely, Verse ships with its own LSP-style tooling and is
  treated as a parallel scripting layer, not as a thing you transpile
  BPs into. In that case bp-reader stays out of the Verse story —
  Verse would have its own MCP server, not extend ours.
- The intermediate case — Verse compiles to a representation that
  the engine can call back into BP from — would still motivate a
  `read_verse_bindings_for(bp)` tool more than a full
  transpile pipeline.

For UE5.7 work, Verse is **out of scope** for any bp-reader
extension.

---

## 4. Comparison table

| Language    | Maturity                                       | Hook surface                                          | AST available?                              | Syntax similarity to BP | Debugger story                                              | Mainline UE supported? | Recommended bp-reader integration |
|-------------|-------------------------------------------------|-------------------------------------------------------|---------------------------------------------|-------------------------|-------------------------------------------------------------|------------------------|------------------------------------|
| AngelScript | Production (Hazelight; shipped It Takes Two)    | Editor + runtime (asset-time UClass synth, hot reload) | Yes (`asIScriptModule` exposes parsed forms; "Not verified" how deep) | High — typed, UFUNCTION-aware, control flow mirrors BP | AS-aware debugger ships with the fork; integrates with Visual Studio Code via an extension | No — Hazelight fork only | **First-class round-trip backend** (mirror CppEmit / CppParse) |
| Lua (UnLua) | Production (Tencent; mobile titles)             | Runtime only (`UObject::ProcessEvent` patch)          | No (Lua source parsed by Lua, no exposed AST) | Low — dynamic, prototype-OO, no static types, no exec/data pin model | UnLua ships a remote debugger ("Not verified" against current head; historically supported) | Yes (plugin)           | **Pretty-printer only** (`EmitLuaFunction`); structured round-trip requires vendoring a Lua parser separately |
| Verse       | UEFN-only; mainline-bound (UE6+, slipped twice) | Verse VM (separate runtime from UObject)              | Yes internally; not exposed for third parties | None — effect types, failable exprs, transactions have no BP/BPIR analog | Verse Debugger in UEFN; no public API for external integration | No (UEFN-only as of 5.7) | **Not feasible**; revisit if Verse ships in mainline with a UClass bridge |

---

## 5. `IScriptBackend` abstraction sketch

Not implemented — sketched here so the extensibility audit (a sibling
research file in this directory) can cross-reference. The shape
mirrors the existing `tools/codegen/CppEmit.h` (emit side) and
`tools/parse/CppParse.h` (parse side), abstracted across languages.

```cpp
// tools/script/IScriptBackend.h  (proposed; not implemented)
//
// Abstracts BPIR ↔ source-language conversions, so adding a new
// target (AngelScript, Lua, Python, JavaScript, ...) is a matter of
// implementing one IScriptBackend subclass and registering it. The
// existing CppEmit / CppParse become an implementation of this
// interface (CppScriptBackend), no behavior change.
//
// Three orthogonal capabilities a backend may or may not support:
//
//   1. Emit:        BPIR  → source string. Always supported.
//   2. Parse:       source → BPIR. Optional (Lua-without-parser case).
//   3. Whole-class: BPIR class doc → header/impl pair on disk.
//      Optional (some targets are single-file).
//
// Backends declare which they support via the capability flags;
// callers (the MCP tool layer) gate tool availability accordingly.

#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bpr::tools::script {

enum class Capability : uint32_t {
    None         = 0,
    EmitFunction = 1 << 0,
    EmitClass    = 1 << 1,
    ParseFunction= 1 << 2,
    ParseClass   = 1 << 3,
    WriteToDisk  = 1 << 4,  // backend can suggest filenames / layouts
};

struct EmitOptions {
    // Backend-specific options carried as JSON. Each backend documents
    // its option schema in a sibling EmitOptionsSchema() method (not
    // shown). Common keys: indent_spaces, mode ("readable"|"compilable"),
    // operator_aliases (bool).
    nlohmann::json options;
};

struct EmitResult {
    std::string source;                        // emitted source
    std::vector<std::string> suggestedFiles;   // for multi-file targets
    nlohmann::json notes = nlohmann::json::array();  // unsupported / approx
};

struct ParseOptions {
    nlohmann::json options;
};

struct ParseResult {
    nlohmann::json bpir;                       // {kind:"function"|"class", ...}
    nlohmann::json notes = nlohmann::json::array();
};

class ScriptBackendError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class NotSupportedError : public ScriptBackendError {
public:
    using ScriptBackendError::ScriptBackendError;
};

class IScriptBackend {
public:
    virtual ~IScriptBackend() = default;

    // Identity.
    virtual std::string Name() const = 0;          // "cpp", "as", "lua", ...
    virtual std::string DisplayName() const = 0;   // "C++", "AngelScript", "Lua"
    virtual std::string FileExtension() const = 0; // ".cpp", ".as", ".lua"
    virtual uint32_t    Capabilities() const = 0;  // bitfield of Capability

    // Emit. Mirrors CppEmit::EmitCppFunction / EmitCppFunctionBody.
    // Throws NotSupportedError if EmitFunction capability not declared.
    virtual EmitResult EmitFunction(const nlohmann::json& bpirFunctionDoc,
                                    const EmitOptions& opts) = 0;

    // Mirrors CppClassEmit::EmitCppClass — full header/impl pair.
    // Throws NotSupportedError if EmitClass capability not declared.
    // For single-file targets (Lua, AS), `headerSource` is empty and
    // the full module goes in `implSource`.
    virtual EmitResult EmitClass(const nlohmann::json& bpirClassDoc,
                                 const EmitOptions& opts) = 0;

    // Parse. Mirrors CppParse::ParseCppFunction.
    // Throws NotSupportedError if ParseFunction capability not declared.
    virtual ParseResult ParseFunction(std::string_view source,
                                      const ParseOptions& opts) = 0;

    // Class-level parse: source for an entire class module → BPIR class
    // doc. Lua-as-pretty-printer never supports this (no AST); C++
    // backend supports it via repeated ParseFunction passes on
    // each member; AS supports it natively.
    virtual ParseResult ParseClass(std::string_view source,
                                   const ParseOptions& opts) = 0;

    // Suggested on-disk layout for an emitted class. Backend returns
    // relative paths (caller resolves against the project's source
    // directory). Single-file targets return one entry; .h/.cpp pairs
    // return two. Useful for write_generated_source-style tools.
    virtual std::vector<std::string> SuggestedFiles(
        const nlohmann::json& bpirClassDoc) const = 0;
};

// Registry. Mirrors the existing ToolRegistry pattern — backends
// register themselves at static-init time and are looked up by name
// from the MCP tool layer.
class ScriptBackendRegistry {
public:
    static ScriptBackendRegistry& Get();

    void Register(std::shared_ptr<IScriptBackend> backend);
    std::shared_ptr<IScriptBackend> Find(std::string_view name) const;
    std::vector<std::shared_ptr<IScriptBackend>> All() const;

private:
    ScriptBackendRegistry() = default;
    // ...
};

}  // namespace bpr::tools::script
```

**Migration of the existing C++ pipeline.** `CppEmit.h` / `CppParse.h`
become the implementation of one `IScriptBackend` (`CppScriptBackend`)
without behavior change. The existing free functions
`EmitCppFunction`, `EmitCppFunctionBody`, `ParseCppFunction` are kept
as thin wrappers that look up the C++ backend through the registry
and call into it — preserves source compatibility for everything that
already imports those headers, while the new path through
`IScriptBackend*` works for the language-agnostic MCP tools.

**MCP tool surface implications.** With `IScriptBackend` in place, the
existing `decompile_function`, `transpile_function`,
`parse_cpp_function`, and `write_generated_source` tools either:

- (a) take a new `target_language` arg defaulting to `"cpp"`, or
- (b) gain `_lang` suffixed variants (`decompile_function_as`,
  `transpile_function_lua`, etc).

(a) is more idiomatic for MCP — fewer tool registrations, smaller
`tools/list` payload, target language as a first-class parameter.
(b) is more discoverable and lets the input schema differ per
language (each backend can advertise its own option schema).
Decision deferred to the extensibility audit.

**What this does *not* abstract.** Asset-side write tools
(`compile_function`, `apply_ops`, the K2-node spawn / wire tools)
stay on the existing K2 path. They are language-agnostic from the
caller's POV — the caller has already converted to BPIR before
hitting them. `IScriptBackend` is purely the BPIR ↔ source layer.

---

## See also

- `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Bpir.h`
  — current BPIR schema; any backend lowers to / parses from this.
- `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/codegen/CppEmit.h`
  — the shape any new emit backend mirrors.
- `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/parse/CppParse.h`
  — the shape any new parse frontend mirrors.
- `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/tools/Decompile.h`
  — BP → BPIR; language-agnostic, reused by every backend.
- Sibling research notes in this directory — `editor-automation.md`,
  `bp-reader-extensibility-audit.md` (which references `IScriptBackend`
  from §5 above).
