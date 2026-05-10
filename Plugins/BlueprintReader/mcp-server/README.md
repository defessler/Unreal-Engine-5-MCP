# bp-reader-mcp

Standalone MCP server that exposes UE5 BlueprintReader tools over
JSON-RPC stdio. Vendored inside the `BlueprintReader` UE plugin so the
whole thing ships as one unit.

For the full picture — tool surface, build instructions, env-var
contract, client setup — see the **[top-level README](../../../README.md)**
or the **[wiki](https://github.com/defessler/Unreal-Engine-5-MCP/wiki)**.

## Quick build (mock backend, no UE)

```powershell
cd Plugins\BlueprintReader\mcp-server
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\tests\Release\bp-reader-tests.exe   # ~350 cases, <5 s
```

The exe is at `build\Release\bp-reader-mcp.exe`. Third-party deps
(nlohmann_json, fmt, doctest) are vendored under `third_party/` —
no git / network / vcpkg required.

## Layout

```
mcp-server/
├── src/
│   ├── BlueprintReaderTypes.h     wire types (snake_case JSON)
│   ├── jsonrpc/                   Server + Mcp (initialize / tools/call / ...)
│   ├── tools/                     ToolRegistry, BlueprintTools, Bpir, Decompile
│   │   ├── codegen/               BPIR → C++ (CppEmit, CppClassEmit, ...)
│   │   └── parse/                 C++ → BPIR (CppLex, CppParse)
│   └── backends/                  Mock, Commandlet, Live, Auto, Caching, ReadOnly
├── tests/                         doctest cases (mock + live commandlet)
├── scripts/                       JSON-RPC + smoke harnesses
├── fixtures/                      BP_Enemy / BP_Pickup / BP_PlayerController
└── third_party/                   vendored deps
```
