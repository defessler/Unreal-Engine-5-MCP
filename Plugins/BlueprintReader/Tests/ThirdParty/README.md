# Vendored dependencies

These are checked in directly so a fresh clone builds with **zero
external network access** — no `git clone`, no FetchContent, no vcpkg.
Drop the plugin into a project, run UBT, and the MCP server compiles.

The headers are exposed to `BlueprintReaderMcpCore` (and the
`BlueprintReaderMcpTests` binary) via `PrivateIncludePaths` entries in
`Tests/BlueprintReaderMcpCore/BlueprintReaderMcpCore.Build.cs`. Each
library is consumed header-only — there's no static-lib or DLL to
build or link separately.

## What's here

| Library         | Version  | Mode          | Source                                              | License |
|-----------------|----------|---------------|------------------------------------------------------|---------|
| nlohmann_json   | v3.11.3  | single header | https://github.com/nlohmann/json                    | MIT     |
| fmt             | 10.2.1   | header-only ¹ | https://github.com/fmtlib/fmt                       | MIT     |
| doctest         | v2.4.11  | single header | https://github.com/doctest/doctest                  | MIT     |

¹ fmt is configured with `FMT_HEADER_ONLY=1` so we don't need to ship its
`src/format.cc` + `src/os.cc`. Slight compile-time hit on TUs that
include `<fmt/core.h>`, no runtime difference.

License files live next to each library's source. All three are
permissive MIT — vendoring is fine for any downstream use.

## Updating

When upstream cuts a new version you want to take:

1. Download the release tarball from the upstream repo (manually, on a
   machine that does have internet).
2. Replace the relevant subtree under `Tests/ThirdParty/`:
   - `nlohmann_json/nlohmann/json.hpp` (and `json_fwd.hpp`) from
     `single_include/nlohmann/`
   - `fmt/fmt/*.h` from `include/fmt/`
   - `doctest/doctest/doctest.h` from `doctest/`
3. Replace the `LICENSE*` file alongside if it changed.
4. Update the version + git tag in the table above.
5. Rebuild the two Program targets via the wrapper:
   `Plugins\BlueprintReader\Scripts\Build-MCPServer.ps1`, then run
   `Binaries\Win64\BlueprintReaderMcpTests.exe` to confirm nothing
   broke.

Keep the directory layout exactly as the include paths in
`BlueprintReaderMcpCore`'s sources expect:

```
#include <nlohmann/json.hpp>     →  Tests/ThirdParty/nlohmann_json/nlohmann/json.hpp
#include <fmt/core.h>            →  Tests/ThirdParty/fmt/fmt/core.h
#include <doctest/doctest.h>     →  Tests/ThirdParty/doctest/doctest/doctest.h
```
