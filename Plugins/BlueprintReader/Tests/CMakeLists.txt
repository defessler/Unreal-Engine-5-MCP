# CMakeLists.txt — standalone build for the bp-reader MCP server + doctest
# suite, for hosts where UBT can't build the UE Program targets (an
# INSTALLED/Launcher engine rejects Program targets:
# "Program targets are not currently supported from this engine distribution").
#
# The MCP server + tests are pure C++20 (bCompileAgainstEngine=false, vendored
# json/fmt/doctest) so they need no engine at all — this recovers the original
# CMake build (removed in the UBT migration, commit 74ac475f) remapped to the
# current Tests/ layout. Sources are GLOBbed (the file list drifted since the
# migration); reconfigure after adding/removing a .cpp.
#
# Build (from this dir's parent or anywhere):
#   cmake -S Plugins/BlueprintReader/Tests -B <build> -G Ninja
#   cmake --build <build>
# Outputs BlueprintReaderMcp.exe + BlueprintReaderMcpTests.exe into
# <repo>/Binaries/Win64 (where CLAUDE.md + .mcp.json expect them).

cmake_minimum_required(VERSION 3.23)
project(bp-reader-mcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Land the exes where the docs/.mcp.json expect (repo Binaries/Win64).
# This file is at <repo>/Plugins/BlueprintReader/Tests.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REPO_ROOT}/Binaries/Win64")

if(MSVC)
    # /W4 (not warnings-as-errors here — the UBT build is the style gate;
    # this fallback just needs a working binary). /bigobj for the large TUs
    # (BlueprintTools.cpp). /EHsc for nlohmann/doctest exceptions.
    # /Zc:preprocessor: the conforming preprocessor (UBT enables this by
    # default). Required — the legacy MSVC preprocessor mis-tokenizes raw
    # string literals (R"(...\"...)") passed as macro arguments, which the
    # doctest CHECK(...) cases in test_cpp_class.cpp rely on.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /bigobj)
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

# --- vendored, header-only deps (Tests/ThirdParty/) ---
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${TP}/nlohmann_json")
add_library(fmt INTERFACE)
target_include_directories(fmt INTERFACE "${TP}/fmt")
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY=1)
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${TP}/doctest")

# --- core static lib (everything under BlueprintReaderMcpCore/Private) ---
set(CORE_PRIV "${CMAKE_CURRENT_SOURCE_DIR}/BlueprintReaderMcpCore/Private")
file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS "${CORE_PRIV}/*.cpp")
add_library(bp-reader-core STATIC ${CORE_SRC})
target_include_directories(bp-reader-core PUBLIC "${CORE_PRIV}")
target_link_libraries(bp-reader-core PUBLIC nlohmann_json fmt)

# --- main server exe ---
add_executable(BlueprintReaderMcp
    "${CMAKE_CURRENT_SOURCE_DIR}/BlueprintReaderMcp/Private/main.cpp")
target_link_libraries(BlueprintReaderMcp PRIVATE bp-reader-core)

# --- doctest suite (everything under BlueprintReaderMcpTests/Private) ---
set(TEST_PRIV "${CMAKE_CURRENT_SOURCE_DIR}/BlueprintReaderMcpTests/Private")
file(GLOB_RECURSE TEST_SRC CONFIGURE_DEPENDS "${TEST_PRIV}/*.cpp")
add_executable(BlueprintReaderMcpTests ${TEST_SRC})
target_include_directories(BlueprintReaderMcpTests PRIVATE "${TEST_PRIV}")
target_link_libraries(BlueprintReaderMcpTests PRIVATE bp-reader-core doctest)

# Stage fixtures next to both exes (loader looks for ./fixtures by the exe).
set(FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/BlueprintReaderMcpTests/fixtures")
foreach(tgt BlueprintReaderMcp BlueprintReaderMcpTests)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FIXTURES}" "$<TARGET_FILE_DIR:${tgt}>/fixtures"
        COMMENT "Staging fixtures next to ${tgt}")
endforeach()

enable_testing()
add_test(NAME bp-reader-tests COMMAND BlueprintReaderMcpTests)
