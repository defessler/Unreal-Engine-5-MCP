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
# Dependencies: prefer find_package (vcpkg), fall back to FetchContent.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# nlohmann/json, fmt, and doctest (release tags below) declare
# cmake_minimum_required versions that CMake 4 rejects (compat with <3.5 was
# dropped). Force a modern policy floor for fetched subprojects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# nlohmann_json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package; fetching v3.11.3.")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# fmt
find_package(fmt CONFIG QUIET)
if(NOT fmt_FOUND)
    message(STATUS "fmt not found via find_package; fetching 10.2.1.")
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 10.2.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(fmt)
endif()

# doctest (header-only test framework)
find_package(doctest CONFIG QUIET)
if(NOT doctest_FOUND)
    message(STATUS "doctest not found via find_package; fetching v2.4.11.")
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(doctest)
endif()

# Note: cpp-subprocess is declared in vcpkg.json but not consumed in Phase 0.
# Phase 1's CommandletBlueprintReader will need it.

# ---------------------------------------------------------------------------
# Library target with the server's guts (linked into both exe and tests).
# ---------------------------------------------------------------------------
add_library(bp-reader-core STATIC
    src/jsonrpc/Server.cpp
    src/jsonrpc/Mcp.cpp
    src/tools/ToolRegistry.cpp
    src/tools/BlueprintTools.cpp
    src/backends/MockBlueprintReader.cpp
    src/backends/BackendFactory.cpp
)
target_include_directories(bp-reader-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../Shared
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
