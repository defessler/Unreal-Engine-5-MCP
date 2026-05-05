// Shared helpers — locate the staged fixtures directory next to the test exe.
#pragma once

#include <filesystem>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace bpr::test {

inline std::filesystem::path TestExecutableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

inline std::filesystem::path FixturesDir() {
    return TestExecutableDir() / "fixtures";
}

} // namespace bpr::test
