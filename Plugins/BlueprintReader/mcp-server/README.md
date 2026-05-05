# bp-reader-mcp

Standalone MCP server that exposes UE5 BlueprintReader tools to Claude over
JSON-RPC stdio. Phase 0 is mock-backed — fixtures only, no engine required.

Tool surface (frozen v0 contract; matches `PLAN.md`):

```
list_blueprints(path = "/Game") -> BPAssetSummary[]
read_blueprint(asset_path)      -> BPMetadata
get_graph(asset_path,
          graph_name = "EventGraph") -> BPGraph
get_function(asset_path,
             function_name)     -> BPFunction
list_variables(asset_path)      -> BPVariable[]
find_node(asset_path, query)    -> BPNode[]
```

Canonical JSON shapes are declared in `src/BlueprintReaderTypes.h`.

## Build

Prereqs: Windows, Visual Studio 2022 (MSVC v143+), CMake 3.23+. No vcpkg,
no git, no network — dependencies (nlohmann_json, fmt, doctest) are
vendored under `third_party/`. If you'd rather pull from vcpkg, set
`VCPKG_ROOT` and pass `--toolchain "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"`
to the configure step; `vcpkg.json` declares the same set.

```powershell
cd D:\Projects\UE5_AI_BP\mcp-server
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
```

Output: `build\Release\bp-reader-mcp.exe`. Fixtures are staged next to it as
`build\Release\fixtures\`.

## Test

```powershell
ctest --preset windows-msvc-release
# or run the binary directly:
.\build\tests\Release\bp-reader-tests.exe
```

## Run / debug

The server reads JSON-RPC 2.0 (LSP-style `Content-Length:` framing) on stdin
and writes responses on stdout. Diagnostic logs go to stderr — never stdout.

Backend selection (env var):

| `BP_READER_BACKEND` | Phase | Behavior                                               |
|---------------------|-------|--------------------------------------------------------|
| `mock` (default)    | 0     | Loads fixtures from `BP_READER_FIXTURES_DIR`           |
| `commandlet`        | 1     | Returns "not implemented in Phase 0" until Phase 1     |
| `live`              | 2     | Returns "not implemented in Phase 0" until Phase 2     |

`BP_READER_FIXTURES_DIR` defaults to `<exe-dir>\fixtures`.

## Register with Claude Desktop / Cowork

Paste this into your MCP client config:

```json
{
    "mcpServers": {
        "bp-reader": {
            "command": "D:\\Projects\\UE5_AI_BP\\mcp-server\\build\\Release\\bp-reader-mcp.exe",
            "env": {
                "BP_READER_BACKEND": "mock"
            }
        }
    }
}
```

Claude Desktop config lives at `%APPDATA%\Claude\claude_desktop_config.json`.
After saving, restart Claude Desktop. The server should appear in the MCP
tools list with `list_blueprints`, `read_blueprint`, `get_graph`,
`get_function`, `list_variables`, `find_node`.

## Smoke-test the stdio loop

PowerShell — frame three messages and pipe them in:

```powershell
$frames = @()
function Add-Frame($obj) {
    $json = $obj | ConvertTo-Json -Depth 8 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetByteCount($json)
    $script:frames += "Content-Length: $bytes`r`n`r`n$json"
}
Add-Frame @{ jsonrpc = "2.0"; id = 1; method = "initialize";
             params = @{ protocolVersion = "2024-11-05";
                         capabilities = @{};
                         clientInfo = @{ name = "smoke"; version = "0" } } }
Add-Frame @{ jsonrpc = "2.0"; method = "notifications/initialized" }
Add-Frame @{ jsonrpc = "2.0"; id = 2; method = "tools/call";
             params = @{ name = "list_blueprints"; arguments = @{ path = "/Game" } } }

$frames -join "" | .\build\Release\bp-reader-mcp.exe
```

Expected: two framed responses (initialize result, then tools/call result
with `isError: false` and a `content[0].text` containing a JSON array of
three asset summaries).

## Layout

```
mcp-server/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── src/
│   ├── main.cpp
│   ├── jsonrpc/
│   │   ├── Server.{h,cpp}      framing + dispatch
│   │   └── Mcp.{h,cpp}          initialize / tools/list / tools/call
│   ├── tools/
│   │   ├── ToolRegistry.{h,cpp}
│   │   └── BlueprintTools.{h,cpp}
│   └── backends/
│       ├── IBlueprintReader.h
│       ├── MockBlueprintReader.{h,cpp}
│       └── BackendFactory.{h,cpp}
├── fixtures/                    BP_Enemy / BP_PlayerController / BP_Pickup
└── tests/                       doctest unit + integration tests
```
