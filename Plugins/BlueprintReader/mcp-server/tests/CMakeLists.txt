# Tests use the same library as the executable. doctest provides its own
# main via DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN in test_main.cpp.

add_executable(bp-reader-tests
    test_main.cpp
    test_types.cpp
    test_jsonrpc.cpp
    test_mcp.cpp
    test_mock_backend.cpp
    test_tools.cpp
    test_commandlet_backend.cpp
    test_framing_stress.cpp
    test_soak.cpp
    test_diagnostics.cpp
    test_protocol_negotiation.cpp
    test_json_projection.cpp
    test_caching_reader.cpp
    test_type_shorthand.cpp
    test_apply_ops.cpp
    test_read_only.cpp
    test_live_backend.cpp
    test_bpir.cpp
    test_decompile.cpp
    test_cpp_codegen.cpp
    test_cpp_class.cpp
    test_unsupported_treatment.cpp
    test_cpp_lex.cpp
    test_cpp_parse.cpp
    test_transpile_roundtrip.cpp
    test_auto_backend.cpp
    test_single_instance_lock.cpp
)
target_link_libraries(bp-reader-tests PRIVATE bp-reader-core)
target_include_directories(bp-reader-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# doctest target name differs depending on whether find_package or
# FetchContent supplied it. Both export `doctest::doctest`.
target_link_libraries(bp-reader-tests PRIVATE doctest::doctest)

# Stage fixtures next to the test exe so the loader can find them with the
# same relative path convention used by the production exe.
add_custom_command(TARGET bp-reader-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
        "$<TARGET_FILE_DIR:bp-reader-tests>/fixtures"
    COMMENT "Staging fixtures next to bp-reader-tests"
)

add_test(NAME bp-reader-tests COMMAND bp-reader-tests)
