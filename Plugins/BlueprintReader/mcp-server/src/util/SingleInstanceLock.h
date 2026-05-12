// SingleInstanceLock — RAII OS-level exclusive lock keyed on the project
// path. Used to enforce "one bp-reader-mcp.exe per .uproject" so multiple
// MCP clients (Claude Code + Claude Desktop + Copilot, common dev setup)
// don't each spawn their own server + editor daemon competing for the
// same .uasset files.
//
// Lock file lives at `<system-temp>/bp-reader-mcp-<hash>.lock` where
// <hash> is an FNV-1a of the absolute project path. Crash-safe: when the
// process exits (clean or via OS termination) the kernel releases the
// file handle and the next instance picks it up. No stale-lock cleanup
// needed.
//
// Implementation: Windows uses CreateFileW with dwShareMode=0 (exclusive
// open). POSIX uses open() + flock(LOCK_EX | LOCK_NB). Both return
// immediately on contention — we never wait, the caller decides whether
// to retry or exit.
//
// Usage:
//   util::SingleInstanceLock lock("D:/Projects/Foo/Foo.uproject");
//   if (!lock.IsHeld()) {
//       std::cerr << "another instance owns " << lock.LockPath() << "\n";
//       return 1;
//   }
//   // ... lock holds for the lifetime of the variable.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bpr::util {

class SingleInstanceLock {
public:
    // Acquire (non-blocking). On failure to acquire (another instance
    // owns the lock), IsHeld() returns false but the object is still
    // usable — caller can inspect LockPath() and OwnerPid() for
    // diagnostics. The OS-level handle is released in the destructor.
    explicit SingleInstanceLock(const std::filesystem::path& projectPath);
    ~SingleInstanceLock();

    SingleInstanceLock(const SingleInstanceLock&) = delete;
    SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;

    bool IsHeld() const { return held_; }

    // Path the lock file was opened at — useful for the user-facing
    // "another instance is using <path>" diagnostic.
    const std::filesystem::path& LockPath() const { return lockPath_; }

    // PID written to the lock by the holding process. Best-effort: the
    // holder writes its PID after acquiring; on contention this read
    // happens after the failed acquire so we may race with the holder's
    // write. Returns nullopt if the lock file doesn't carry a parseable
    // PID — treat as "another instance, unknown PID."
    std::optional<int> OwnerPid() const;

private:
    std::filesystem::path lockPath_;
    bool held_ = false;
    // Opaque handle: HANDLE on Windows, int fd on POSIX. void* is large
    // enough for either; the .cpp does the type-specific casts.
    void* handle_ = nullptr;
};

} // namespace bpr::util
