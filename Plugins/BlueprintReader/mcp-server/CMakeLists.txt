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
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
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
    src/backends/MockBlueprintReader.cpp
    src/backends/CommandletBlueprintReader.cpp
    src/backends/CachingBlueprintReader.cpp
    src/backends/ReadOnlyBlueprintReader.cpp
    src/backends/LiveBlueprintReader.cpp
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
set_target_properties(bp-reader-mcp PROPERTIES OUTPUT_NAME "bp-reader-mcp")

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
