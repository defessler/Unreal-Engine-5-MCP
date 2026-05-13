# 04 — The standalone MCP server

The other doc, [03-plugin-internals.md](03-plugin-internals.md),
covers what runs inside the UE editor. This one covers what runs
outside it — the `bp-reader-mcp.exe` process that MCP clients
actually launch. Component relationships are in
[02-architecture.md](02-architecture.md).

## Why standalone

UE plugins can't host stdio servers cleanly. The editor process owns
stdout for its log device, the commandlet entry points don't keep
running between calls, and shipping the editor on every developer's
machine is a non-starter for "I want Copilot in VS Code to read my
blueprints". The MCP server is a 4-MB executable with no UE
dependency, vendored deps, and a fixed surface.

The trade is that anything the server learns about a `.uasset` has to
come from a child process or socket connection to UE. That's the
whole `IBlueprintReader` story.

## Source layout

```
mcp-server/
├── src/
│   ├── main.cpp                     entry point + doctor/config subcommands
│   ├── Env.{h,cpp}                  env-var helpers
│   ├── Diagnostics.{h,cpp}          setup checks (doctor + startup)
│   ├── BlueprintReaderTypes.h       wire shapes (shared with the plugin)
│   ├── jsonrpc/
│   │   ├── Server.{h,cpp}           JSON-RPC 2.0 transport
│   │   └── Mcp.{h,cpp}              MCP handshake + tools/call wrapper
│   ├── tools/
│   │   ├── ToolRegistry.{h,cpp}     descriptor + dispatch table
│   │   ├── BlueprintTools.{h,cpp}   116 tool registrations
│   │   ├── ApplyOps.{h,cpp}         apply_ops + preview_ops
│   │   ├── CompileFunction.{h,cpp}  compile_function
│   │   ├── JsonProjection.{h,cpp}   `fields` projection + `limit`/`offset`
│   │   ├── TypeShorthand.{h,cpp}    "Vector" → BPPinType expansion
│   │   ├── Bpir.{h,cpp}             BPIR AST (BP↔C++ pivot)
│   │   ├── Decompile.{h,cpp}        BP graph → BPIR
│   │   ├── codegen/                  BPIR → C++ emission
│   │   └── parse/                    C++ → BPIR (lexer + parser)
│   ├── backends/
│   │   ├── IBlueprintReader.h       the contract
│   │   ├── BackendFactory.{h,cpp}   config + Create()
│   │   ├── MockBlueprintReader.{h,cpp}
│   │   ├── CommandletBlueprintReader.{h,cpp}
│   │   ├── LiveBlueprintReader.{h,cpp}
│   │   ├── AutoBlueprintReader.{h,cpp}
│   │   ├── CachingBlueprintReader.{h,cpp}
│   │   └── ReadOnlyBlueprintReader.{h,cpp}
│   └── util/
│       └── SingleInstanceLock.{h,cpp}
├── tests/                           doctest cases (~350 mock + live)
├── scripts/                         JSON-RPC + smoke harnesses
├── fixtures/                        BP_*.json mock data
├── third_party/
│   ├── nlohmann_json/
│   ├── fmt/
│   └── doctest/
├── CMakeLists.txt
└── vcpkg.json                       declared but not consumed by default
```

## Entry point

`main.cpp` has three modes:

```cpp
int main(int argc, char** argv) {
    // ...
    if (!args.empty()) {
        if (a == "doctor") return RunDoctor();
        if (a == "config") return RunConfig(...);
        if (a == "--help") { PrintUsage(...); return 0; }
        return 2;
    }
    return RunServerLoop();
}
```

`RunServerLoop` is the production path
(`main.cpp:247-335`):

1. **Force binary stdio**
   (`main.cpp:69-85`). Windows defaults stdin/stdout to text mode,
   which mangles framing (CRLF translation, Ctrl-Z = EOF). Two layers
   of buffering also need disabling: iostream's filebuf gets
   `unitbuf`, and the C runtime's stdout buffer is set to `_IONBF`.
   Without this, responses sit in the CRT buffer until the next
   request arrives and clients hit their handshake timeout.

2. **Load environment** via `backends::ConfigFromEnv(exeDir, std::cerr)`
   — env vars + path auto-discovery from the exe location
   (`BackendFactory.cpp:73-194`).

3. **Acquire single-instance lock**
   (`main.cpp:267-290`). Project-keyed; see "Single-instance lock"
   below.

4. **Run setup checks** (`diag::RunSetupChecks`). Logged immediately
   so users see actionable hints instead of a silent hang.

5. **Build the backend** via `backends::Create(cfg)`. Wraps the
   chosen backend with `CachingBlueprintReader` (TTL + mtime
   invalidation) and optionally `ReadOnlyBlueprintReader`
   (`BackendFactory.cpp:256-265`).

6. **Register tools** via `tools::RegisterBlueprintTools`,
   `RegisterApplyOps`, `RegisterCompileFunction`.

7. **Register MCP handlers** via `mcp::RegisterHandlers`.

8. **Block on `server.Run(std::cin, std::cout, std::cerr)`** until
   stdin closes.

The `doctor` and `config` subcommands write to stdout-text and
stderr; they're explicitly NOT the JSON-RPC transport.

## JSON-RPC layer

### Framing

`bpr::jsonrpc::ReadFrame`
(`Server.cpp:117-159`) auto-detects framing from the first
non-whitespace byte:

- `{` or `[` → newline-delimited (the MCP spec)
- anything else → LSP-style `Content-Length` headers

The detected format is locked in for the rest of the session
(`Server.cpp:285-292`). `WriteFrame` mirrors it
(`Server.cpp:161-171`). Two clients with different framings can talk
to the same server binary without configuration.

UTF-8 BOM at the start of the stream is tolerated
(`Server.cpp:130-144`) — some Windows clients prepend one.

### Dispatch

`Server::Dispatch(body)` (`Server.cpp:202-262`) is the synchronous
heart:

```cpp
1. Validate body is a JSON object.
2. Extract `id` (or treat as notification if absent).
3. Validate "jsonrpc" == "2.0".
4. Extract "method".
5. Default "params" to {}.
6. Look up handler.
7. Call handler; catch std::exception → InternalError.
8. Build result or error envelope.
```

Errors are mapped to JSON-RPC standard codes:

```cpp
enum class ErrorCode : int {
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,
};
```

Per JSON-RPC 2.0, parse errors get an envelope with `id=null`
(`Server.cpp:297-304`). Notifications (no `id`) get no response, ever
— including failures.

### Batches

Batched requests (an array of bodies) are dispatched in order, and
the response is the array of non-null responses
(`Server.cpp:309-327`). MCP doesn't use batches in practice; this
exists because JSON-RPC requires it.

### Response model

Handlers return a `Response`:

```cpp
struct Response {
    std::optional<nlohmann::json> result;
    std::optional<Error> error;
    static Response Ok(nlohmann::json result);
    static Response Fail(ErrorCode code, std::string message, ...);
};
```

Two static factories make the call sites readable.

## MCP handshake

`mcp::RegisterHandlers` (`Mcp.cpp:37-160`) wires four endpoints onto
the generic `jsonrpc::Server`:

### `initialize`

Negotiates the protocol version. Accepts the client's
`protocolVersion` if it matches one of the known revisions,
otherwise falls back to the server's default
(`Mcp.cpp:41-75`):

```cpp
static const std::vector<std::string> kKnownVersions = {
    "2024-11-05", // initial public spec
    "2025-03-26", // tool annotations / progress
    "2025-06-18", // resources, etc.
};
```

Capabilities advertised: `{tools: {}}` — we serve tools, no
list-changed notifications. `serverInfo: {name, version}` from
`ServerInfo`.

### `notifications/initialized`

Notification, return value ignored. No-op
(`Mcp.cpp:79-82`).

### `ping`

Returns `{}`. Lets clients keep-alive without a real call
(`Mcp.cpp:85-87`).

### `tools/list`

Returns `{tools: registry.ListSpec()}` — the descriptor array from
`ToolRegistry` (`Mcp.cpp:90-92`).

### `tools/call`

The interesting one. `Mcp.cpp:95-159`:

```cpp
const auto t0 = std::chrono::steady_clock::now();
auto elapsedMs = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
};

try {
    nlohmann::json toolResult = (*fn)(arguments);
    nlohmann::json meta = {
        {"elapsed_ms", elapsedMs()},
        {"tool", name},
    };
    return jr::Response::Ok(MakeToolTextContent(toolResult.dump(2),
        /*isError=*/false, std::move(meta)));
} catch (const std::exception& e) {
    nlohmann::json meta = {
        {"elapsed_ms", elapsedMs()},
        {"tool", name},
    };
    if (!arguments.empty()) meta["args"] = arguments;
    return jr::Response::Ok(MakeToolTextContent(
        fmt::format("tool error: {}", e.what()), /*isError=*/true,
        std::move(meta)));
}
```

Two structural decisions worth knowing:

- **Tool errors are not JSON-RPC errors.** Per the MCP convention,
  a thrown tool handler becomes a successful JSON-RPC response
  whose `content` includes `isError: true`. JSON-RPC errors are
  reserved for transport-level failures (parse error, malformed
  envelope). This means a failed tool *still* counts as a
  successful round-trip from the client's perspective and the
  agent can surface the error text directly.

- **`_meta.args` on errors.** Successful responses get
  `{elapsed_ms, tool}`. Errors get `{elapsed_ms, tool, args}` —
  the call args are echoed verbatim because for an agent debugging
  a failure, "what did I pass" is exactly the missing context. No
  filtering: this server's tool args don't carry credentials.

The unknown-tool case returns an MCP tool error
(`Mcp.cpp:117-123`), not a JSON-RPC `MethodNotFound` — the MCP
method `tools/call` itself exists, the tool name is just unknown.

## Tool registry

`ToolRegistry` (`tools/ToolRegistry.h`):

```cpp
struct ToolDescriptor {
    std::string name;
    std::string description;
    nlohmann::json input_schema;   // JSON Schema object
};

using ToolFn = std::function<nlohmann::json(const nlohmann::json& arguments)>;

class ToolRegistry {
public:
    void Add(ToolDescriptor desc, ToolFn fn);
    nlohmann::json ListSpec() const;
    const ToolFn* Find(const std::string& name) const;
private:
    std::vector<ToolDescriptor> descriptors_;
    std::map<std::string, ToolFn> fns_;
};
```

Two storage shapes:

- **`descriptors_` (vector)** preserves registration order so
  `tools/list` returns a stable iteration.
- **`fns_` (map)** is the lookup table for `tools/call`.

`ToolRegistry::Add` (`ToolRegistry.cpp:7-20`) has replace-in-place
semantics. Without it, re-registering the same tool name appended a
duplicate descriptor and `tools/list` advertised the same tool
twice. (Replace-in-place is also what lets test code re-register a
tool with a mocked handler.)

### Handler shape

A handler receives the `arguments` object from the `tools/call`
request and returns arbitrary JSON. May throw any `std::exception`
subclass; `mcp::RegisterHandlers` catches it.

A read-tool handler looks like this
(`BlueprintTools.cpp:156-163`, the `list_blueprints` body):

```cpp
registry.Add(std::move(d), [&reader](const nlohmann::json& args) {
    std::string path = OptString(args, "path", "/Game");
    auto ctl = ParseResponseControls(args);
    auto items = reader.ListBlueprints(path);
    nlohmann::json body = items;
    ApplyResponseControls(body, ctl);
    return body;
});
```

The lambda captures `reader` by reference (the `IBlueprintReader`
instance from `BackendFactory::Create`). All write tools follow the
same pattern with different `reader` method calls and without the
response-controls block (writes don't need pagination).

## Response controls (`fields`, `limit`, `offset`)

Read tools accept three optional shared arguments
(`BlueprintTools.cpp:77-130`):

- `fields`: array of dotted field paths. The example given to clients:
  `[\"name\", \"variables[].name\"]` returns just the BP name and
  variable names.
- `limit`: optional integer cap on array length.
- `offset`: 0-based offset into the result array.

`ParseResponseControls` reads all three out of the args
(`BlueprintTools.cpp:110-118`). `ApplyResponseControls` then mutates
the response body
(`BlueprintTools.cpp:119-130`):

```cpp
void ApplyResponseControls(nlohmann::json& body, const ResponseControls& ctl) {
    if (body.is_array() && (ctl.offset > 0 || ctl.limit >= 0)) {
        std::size_t off = std::min<std::size_t>(ctl.offset, body.size());
        std::size_t end = (ctl.limit < 0)
                              ? body.size()
                              : std::min<std::size_t>(off + ctl.limit, body.size());
        nlohmann::json sliced = nlohmann::json::array();
        for (std::size_t i = off; i < end; ++i) sliced.push_back(std::move(body[i]));
        body = std::move(sliced);
    }
    ApplyProjection(body, ctl.fields);
}
```

The order matters: paginate first (cheap), then project (per
element) — the opposite order would project rows we're about to
discard.

Field projection lives in `tools/JsonProjection.cpp`. Two flavors:

- **Bare keys** (`["asset_path"]`) on an array body: applied
  per-element. `ApplyProjection` detects the top-level array and
  iterates (`JsonProjection.cpp:117-120`).
- **Explicit `[]`** in a path (`variables[].name`): descends through
  the array element-wise.

Schema entries are emitted by the shared helpers `FieldsProperty()`,
`LimitProperty()`, `OffsetProperty()`
(`BlueprintTools.cpp:77-101`) so every tool's `tools/list` schema
documents these the same way.

## Backend selection — recap

`BackendFactory::Create` (`BackendFactory.cpp:196-266`) chains three
wrappers around the chosen concrete reader:

```cpp
auto cached = WrapWithCache(buildInner(),
                            std::chrono::seconds(cfg.cacheTtlSeconds),
                            cfg.uproject);
return MaybeWrapReadOnly(std::move(cached), cfg.readOnly);
```

The cache uses both TTL and `.uasset` mtime for invalidation —
walking the project directory lets a read-after-write see the new
state without waiting for the TTL. Read-only wraps outermost so
writes fail-fast with a clear error instead of going through the
cache and then hitting the backend.

`mock|commandlet|live|auto` selection logic at
`BackendFactory.cpp:196-256`. The default when no
`BP_READER_BACKEND` is set is determined at config time
(`BackendFactory.cpp:189-191`): `auto` if a `.uproject` is
discoverable, `mock` otherwise. See
[03-plugin-internals.md → "Live TCP server"](03-plugin-internals.md#live-tcp-server)
for what the live backend connects to.

## Single-instance lock

Two `bp-reader-mcp.exe` processes against the same project would
spawn two commandlet daemons; both would hold open the same
`.uasset` files; one would lose `SavePackage` to a sharing
violation; DDC corruption is on the table. The lock prevents it.

`util::SingleInstanceLock` (`util/SingleInstanceLock.cpp`):

```cpp
SingleInstanceLock instanceLock(cfg.uproject);
if (!instanceLock.IsHeld() && !allowMulti) { /* refuse to start */ }
```

The lock path is derived from the project path via FNV-1a 64
(`SingleInstanceLock.cpp:25-60`):

```cpp
uint64_t Fnv1a64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}
```

The path is lowercased on Windows so `C:\Foo` and `c:\foo` resolve
to the same key, then weakly canonicalized, then hashed. The lock
file is `%TEMP%/bp-reader-mcp-<hex>.lock`.

On Windows the lock is taken via `CreateFileW` with `dwShareMode=0`
— exclusive open. Subsequent opens from another process get
`ERROR_SHARING_VIOLATION`. The PID is written into the file body
(`SingleInstanceLock.cpp:67-108`) so diagnostics can name the
holder, though Windows' exclusive open denies the read from a
competing process until the holder closes. The path string is
exposed via `LockPath()` so the error message can show the user
which file to inspect.

POSIX uses `flock(LOCK_EX | LOCK_NB)` on `O_CREAT|O_RDWR` instead
(`SingleInstanceLock.cpp:109-132`). The kernel cleans up
automatically on `SIGKILL`.

`BP_READER_ALLOW_MULTI=1` skips the check — caller is responsible
for not wedging the project state.

## Build

`CMakeLists.txt:1-122`. Three rules worth knowing:

### Vendored deps, no network

Lines 35-61:

```cmake
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)

add_library(fmt INTERFACE)
target_include_directories(fmt INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt)
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY=1)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
```

All three deps are header-only when consumed this way. fmt uses
`FMT_HEADER_ONLY=1` to keep template instantiations inline so we
don't need `src/format.cc` + `src/os.cc`. A fresh clone builds with
no `git submodule`, no `FetchContent`, no `vcpkg install`, no
network access — which is what makes the
`BlueprintReader.uplugin` `PreBuildSteps` reliable (UE plugin
PreBuild runs in a sandbox where network access isn't guaranteed).

### Warnings-as-errors for unused parameters

Lines 12-33:

```cmake
add_compile_options(/we4100 /we4101 /we4189 /we4505)
```

The motivating case was the `visited` parameter in
`DecompileStatement` — clean at MSVC `/W4` here but flagged as a
warning in a downstream consumer's stricter build. Promoting the
relevant warnings to errors makes us catch them on this side.

### Fixtures staging

Lines 111-116:

```cmake
add_custom_command(TARGET bp-reader-mcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        "$<TARGET_FILE_DIR:bp-reader-mcp>/fixtures"
    COMMENT "Staging fixtures next to bp-reader-mcp.exe"
)
```

The mock backend's default fixture path is `<exe>/fixtures`. Post-
build copy makes the default work without env vars.

### `vcpkg.json` — declared but not consumed

`mcp-server/vcpkg.json` exists and lists `nlohmann-json`, `fmt`,
`doctest` as dependencies. It's not used by the default CMake
configuration — `find_package` is never called for these names. The
manifest is kept for the day someone wants a non-vendored build
(corporate compliance, security review needing fresh CVE pulls,
etc.); flipping the CMakeLists to `find_package(...)` instead of
the vendored `INTERFACE` libraries is a contained change.

## Testing

`tests/test_tools.cpp:32-35` and `tests/test_mcp.cpp:90` both pin
`spec.size() == 119`. Bumping the tool count requires updating
both. The descriptive comment on `test_tools.cpp:32` also names the
21 categories — keep it in sync with new registrations:

```cpp
TEST_CASE("ToolRegistry exposes 119 tools (12 read + 22 write + 3 meta + 3 batch
    + 3 transpile + 9 project/asset + 12 live editor + 1 automation + 7 material
    + 5 widget + 5 BT + 4 DataAsset + 5 StateTree + 4 profile + 2 cook
    + 3 class info + 4 viewport + 4 Niagara + 4 Sequencer + 3 GAS + 4 AnimGraph)
    with input schemas")
```

Test layout: doctest cases live in `mcp-server/tests/`. Mock-only
runs need no env (these are the CI default). Live cases auto-skip
when `BP_READER_PROJECT` / `BP_READER_ENGINE_DIR` aren't set;
setting both enables the live integration suite. CI runs only the
mock subset; live cases run locally on developer machines.

## See also

- [01-overview.md](01-overview.md) — both halves at a glance.
- [02-architecture.md](02-architecture.md) — process model, request
  lifecycle, threading.
- [03-plugin-internals.md](03-plugin-internals.md) — the UE-side
  counterpart of every protocol described here.
- `mcp-server/scripts/roundtrip.ps1` — shows end-to-end framing
  against a running `bp-reader-mcp.exe`.
- `mcp-server/tests/test_mcp.cpp` — the canonical example of an
  in-process MCP handshake + `tools/list` + `tools/call`.
