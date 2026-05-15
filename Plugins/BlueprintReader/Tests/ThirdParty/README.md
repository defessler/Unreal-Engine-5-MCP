# Vendored dependencies

These are checked in directly so a fresh clone builds with **zero
external network access** — no `git clone`, no FetchContent, no vcpkg.
Drop the plugin into a project, run UBT, and the MCP server compiles.

The libraries are linked into `bp-reader-core` (and `bp-reader-tests`)
via INTERFACE library targets defined in `mcp-server/CMakeLists.txt`.
Each preserves the upstream target name (`fmt::fmt`, `nlohmann_json::nlohmann_json`,
`doctest::doctest`) so the link lines elsewhere don't care that the
deps are vendored.

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
2. Replace the relevant subtree under `third_party/`:
   - `nlohmann_json/nlohmann/json.hpp` (and `json_fwd.hpp`) from
     `single_include/nlohmann/`
   - `fmt/fmt/*.h` from `include/fmt/`
   - `doctest/doctest/doctest.h` from `doctest/`
3. Replace the `LICENSE*` file alongside if it changed.
4. Update the version + git tag in the table above.
5. Run `cmake --build mcp-server/build --config Release` and the test
   suite to confirm nothing broke.

Keep the directory layout exactly as the include paths in `src/` expect:

```
#include <nlohmann/json.hpp>     →  third_party/nlohmann_json/nlohmann/json.hpp
#include <fmt/core.h>            →  third_party/fmt/fmt/core.h
#include <doctest/doctest.h>     →  third_party/doctest/doctest/doctest.h
```

If you prefer pulling deps from vcpkg instead, the `mcp-server/vcpkg.json`
manifest still declares them — flip the CMakeLists.txt back to
`find_package(... CONFIG)` and remove the INTERFACE-library block.
