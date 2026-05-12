#include "util/SingleInstanceLock.h"

#include <cstdint>
#include <fstream>
#include <string>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <process.h>  // _getpid
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/file.h>
#endif

namespace bpr::util {

namespace {

// FNV-1a 64-bit. Used to derive a stable, filesystem-safe identifier
// from the absolute project path. We don't need crypto strength — just
// "two different projects map to different lock files." Collision
// odds at this scale are vanishingly low.
uint64_t Fnv1a64(std::string_view s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::filesystem::path DeriveLockPath(const std::filesystem::path& projectPath)
{
    // Stable key: canonical absolute project path lowercased on Windows
    // (the filesystem is case-insensitive — same project entered as
    // C:\Foo and c:\foo should share a lock).
    std::string key;
    std::error_code ec;
    auto abs = std::filesystem::weakly_canonical(projectPath, ec);
    if (ec || abs.empty()) abs = projectPath;
    key = abs.string();
#if defined(_WIN32)
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    // Default to a project-less lock when no .uproject was passed
    // (mock backend / no auto-discovery). One server per machine in
    // that case is still the desired default.
    if (key.empty()) key = "bp-reader-mcp-default";

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(Fnv1a64(key)));
    auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) temp = std::filesystem::path(".");
    return temp / (std::string("bp-reader-mcp-") + hex + ".lock");
}

} // namespace

SingleInstanceLock::SingleInstanceLock(const std::filesystem::path& projectPath)
    : lockPath_(DeriveLockPath(projectPath))
{
#if defined(_WIN32)
    // Exclusive open: dwShareMode=0 → any later opener from another
    // process gets ERROR_SHARING_VIOLATION. FILE_FLAG_DELETE_ON_CLOSE
    // would be nice for cleanup but breaks the "read holder's PID
    // from a competing process" path below.
    HANDLE h = CreateFileW(lockPath_.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           /*dwShareMode=*/0,
                           /*lpSecurityAttributes=*/nullptr,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        held_ = false;
        handle_ = nullptr;
        return;
    }
    held_ = true;
    handle_ = h;

    // Truncate then write our PID. Other processes that fail to open
    // exclusively can still open the file read-shared (we hold
    // GENERIC_READ — open-for-read from a different process is denied
    // because dwShareMode=0, but if we widened the share they could
    // peek). For now we rely on the PID being readable via a separate
    // unprivileged path: we periodically refresh PID into a sibling
    // text file. Simpler: store PID inline at acquire time; competing
    // processes won't see it until our handle closes, but
    // diagnostics-only.
    const int pid =
    #if defined(_WIN32)
        static_cast<int>(GetCurrentProcessId());
    #else
        static_cast<int>(::getpid());
    #endif
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d\n", pid);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(h);
    }
#else
    int fd = ::open(lockPath_.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        held_ = false;
        handle_ = nullptr;
        return;
    }
    // Non-blocking exclusive flock. Released automatically when the
    // fd is closed (kernel handles this even on SIGKILL).
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        held_ = false;
        handle_ = nullptr;
        return;
    }
    // Truncate + write PID.
    ::ftruncate(fd, 0);
    const int pid = static_cast<int>(::getpid());
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d\n", pid);
    if (n > 0) ::write(fd, buf, static_cast<size_t>(n));
    held_ = true;
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
}

SingleInstanceLock::~SingleInstanceLock()
{
    if (!held_ || !handle_) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = nullptr;
    held_ = false;
}

std::optional<int> SingleInstanceLock::OwnerPid() const
{
    // Read the PID written by whoever holds the lock. On Windows the
    // exclusive open prevents reads while the holder has the handle,
    // so this only works post-shutdown or when called from the holding
    // process. POSIX flock allows shared reads regardless of lock state.
    std::ifstream in(lockPath_);
    if (!in) return std::nullopt;
    int pid = 0;
    in >> pid;
    if (pid <= 0) return std::nullopt;
    return pid;
}

} // namespace bpr::util
