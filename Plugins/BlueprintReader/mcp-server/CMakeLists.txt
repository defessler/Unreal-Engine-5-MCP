cmake_minimum_required(VERSION 3.23)
project(bp-reader-mcp
    VERSION 0.1.0
    DESCRIPTION "MCP server exposing UE5 BlueprintReader tools over JSON-RPC stdio"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    # Promote unused-variable / unused-parameter warnings to errors so
    # they can't slip through into downstream consumers' build logs.
    # The C4100 case that motivated this was the `visited` parameter in
    # DecompileStatement — it compiled clean here at /W4 but surfaced
    # as a warning in the MyGame consumer's build (different warning
    # configuration). Now /we4100 makes us fail the build, forcing the
    # fix on this side.
    #   C4100: unreferenced formal parameter
    #   C4101: unreferenced local variable
    #   C4189: local variable initialized but not used
    #   C4505: unreferenced local function has been removed
    add_compile_options(/we4100 /we4101 /we4189 /we4505)
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    # GCC / Clang — same intent, different flag spellings.
    add_compile_options(-Wall -Wextra
        -Werror=unused-parameter
        -Werror=unused-variable
        -Werror=unused-function)
endif()

# ---------------------------------------------------------------------------
# Dependencies: vendored under third_party/. No network, no git, no fetch.
# Update by replacing the relevant subtree under third_party/ — see
# third_party/README.md for versions and provenance.
# ---------------------------------------------------------------------------

# nlohmann_json — header-only. We export the same target name FetchContent
# produces (nlohmann_json::nlohmann_json) so the link lines below don't care
# which way the dep is sourced.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

# fmt — vendored as headers only. FMT_HEADER_ONLY makes the templates fully
# inlinable so we don't need to ship src/format.cc + src/os.cc.
add_library(fmt INTERFACE)
target_include_directories(fmt INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt)
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY=1)
add_library(fmt::fmt ALIAS fmt)

# doctest — single header.
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
add_library(doctest::doctest ALIAS doctest)

# ---------------------------------------------------------------------------
# Library target with the server's guts (linked into both exe and tests).
# ---------------------------------------------------------------------------
add_library(bp-reader-core STATIC
    src/Env.cpp
    src/Diagnostics.cpp
    src/jsonrpc/Server.cpp
    src/jsonrpc/Mcp.cpp
    src/tools/ToolRegistry.cpp
    src/tools/BlueprintTools.cpp
    src/tools/JsonProjection.cpp
    src/tools/TypeShorthand.cpp
    src/tools/ApplyOps.cpp
    src/tools/CompileFunction.cpp
    src/tools/Bpir.cpp
    src/tools/Decompile.cpp
    src/tools/codegen/CppEmit.cpp
    src/tools/codegen/CppClassEmit.cpp
    src/tools/codegen/UnsupportedTreatment.cpp
    src/tools/parse/CppLex.cpp
    src/tools/parse/CppParse.cpp
    src/backends/MockBlueprintReader.cpp
    src/backends/CommandletBlueprintReader.cpp
    src/backends/CachingBlueprintReader.cpp
    src/backends/ReadOnlyBlueprintReader.cpp
    src/backends/SocketBlueprintReader.cpp
    src/backends/AutoBlueprintReader.cpp
    src/backends/BackendFactory.cpp
)
target_include_directories(bp-reader-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bp-reader-core PUBLIC
    nlohmann_json::nlohmann_json
    fmt::fmt
)
set_target_properties(bp-reader-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Main executable.
# ---------------------------------------------------------------------------
add_executable(bp-reader-mcp src/main.cpp)
target_link_libraries(bp-reader-mcp PRIVATE bp-reader-core)

if(BP_READER_PROJECT_NAME)
    set(BP_READER_EXE_NAME "bp-reader-mcp-${BP_READER_PROJECT_NAME}")
else()
    set(BP_READER_EXE_NAME "bp-reader-mcp")
endif()
set_target_properties(bp-reader-mcp PROPERTIES OUTPUT_NAME "${BP_READER_EXE_NAME}")

# Always produce a canonical bp-reader-mcp(.exe) alongside the named
# artifact so existing .mcp.json configs referencing the canonical name
# continue to work. Hard-link on Windows (mklink /H — no admin needed);
# create_hardlink on Unix.
if(BP_READER_PROJECT_NAME)
    if(WIN32)
        # mklink is a cmd.exe builtin and rejects forward-slash paths
        # (they look like /switch args), so wrap both paths in
        # $<SHELL_PATH:...> to get native backslashes.
        add_custom_command(TARGET bp-reader-mcp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp.exe"
            COMMAND cmd /c mklink /H "$<SHELL_PATH:$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp.exe>" "$<SHELL_PATH:$<TARGET_FILE:bp-reader-mcp>>"
            COMMENT "Hard-linking canonical bp-reader-mcp.exe to ${BP_READER_EXE_NAME}.exe")
    else()
        add_custom_command(TARGET bp-reader-mcp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_hardlink "$<TARGET_FILE:bp-reader-mcp>" "$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp"
            COMMENT "Hard-linking canonical bp-reader-mcp to ${BP_READER_EXE_NAME}")
    endif()
endif()

# Copy fixtures next to the executable so the default fixture-loading path
# (./fixtures relative to the exe) just works after a build.
add_custom_command(TARGET bp-reader-mcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        "$<TARGET_FILE_DIR:bp-reader-mcp>/fixtures"
    COMMENT "Staging fixtures next to bp-reader-mcp.exe"
)

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
