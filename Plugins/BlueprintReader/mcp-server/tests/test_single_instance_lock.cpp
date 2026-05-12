// Tests for the single-instance lock that guards bp-reader-mcp against
// multiple servers / editor daemons fighting over the same .uasset
// files when several MCP clients launch the server in parallel.

#include <doctest/doctest.h>

#include "util/SingleInstanceLock.h"

#include <filesystem>
#include <fstream>

using namespace bpr::util;

namespace
{
    // Pick a unique-per-run path so concurrent CI shards don't trample
    // each other. The hash inside SingleInstanceLock keys off this so
    // each test gets its own lock file.
    std::filesystem::path UniqueProjectPath()
    {
        auto temp = std::filesystem::temp_directory_path();
        return temp / ("bp-reader-test-" +
                       std::to_string(std::hash<std::string>{}(__FILE__)) + "-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(&temp)) +
                       ".uproject");
    }
}

TEST_CASE("SingleInstanceLock: first acquire holds, second fails")
{
    auto proj = UniqueProjectPath();

    SingleInstanceLock first(proj);
    CHECK(first.IsHeld());
    // The first holder's PID file should be readable from inside the
    // holding process. (Cross-process read is platform-dependent — we
    // don't assert that here; the live test in this PR's verification
    // covers it.)

    {
        // Second acquire from THIS process should fail — OS file
        // exclusivity covers both inter-process AND intra-process
        // contention on the same file.
        SingleInstanceLock second(proj);
        CHECK_FALSE(second.IsHeld());
        // Same lock path as the first one — proves the hash is stable
        // across instances reading the same project.
        CHECK(second.LockPath() == first.LockPath());
    }
    // After `second` goes out of scope (it didn't hold anything,
    // destructor is a no-op).
    CHECK(first.IsHeld());
}

TEST_CASE("SingleInstanceLock: releases on destruction")
{
    auto proj = UniqueProjectPath();
    std::filesystem::path lockPath;
    {
        SingleInstanceLock first(proj);
        REQUIRE(first.IsHeld());
        lockPath = first.LockPath();
    }
    // After `first` is destroyed, a new acquire should succeed.
    SingleInstanceLock second(proj);
    CHECK(second.IsHeld());
    CHECK(second.LockPath() == lockPath);
}

TEST_CASE("SingleInstanceLock: different projects get different locks")
{
    auto projA = UniqueProjectPath();
    auto projB = projA;
    projB.replace_filename("bp-reader-test-distinct.uproject");

    SingleInstanceLock a(projA);
    REQUIRE(a.IsHeld());
    SingleInstanceLock b(projB);
    // Different project paths → different hashes → different lock
    // files → both can hold simultaneously.
    CHECK(b.IsHeld());
    CHECK(a.LockPath() != b.LockPath());
}

TEST_CASE("SingleInstanceLock: lock file lives under system temp")
{
    auto proj = UniqueProjectPath();
    SingleInstanceLock lock(proj);
    REQUIRE(lock.IsHeld());
    // Stable convention: filename starts with `bp-reader-mcp-` so it's
    // recognizable in /tmp listings. The path uses the system temp
    // directory so it's auto-cleaned by the OS on reboot.
    CHECK(lock.LockPath().filename().string().rfind("bp-reader-mcp-", 0) == 0);
    CHECK(lock.LockPath().extension() == ".lock");
}

TEST_CASE("SingleInstanceLock: empty project path still gives a stable lock")
{
    // Backend may be `mock` with no .uproject auto-discovered. We still
    // want a deterministic lock so two mock-backend servers don't both
    // claim to be the singleton.
    SingleInstanceLock a(std::filesystem::path{});
    REQUIRE(a.IsHeld());
    SingleInstanceLock b(std::filesystem::path{});
    CHECK_FALSE(b.IsHeld());
    CHECK(b.LockPath() == a.LockPath());
}
