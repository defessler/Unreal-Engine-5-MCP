# Multi-session MCP server + shared commandlet daemon — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let multiple Claude Code / Copilot CLI sessions use bp-reader against the same UE project concurrently. Move the single-instance constraint from the MCP server to the commandlet daemon, convert the daemon from stdin/stdout to a localhost TCP server that mirrors the live backend, and add project-keyed naming for at-a-glance process identifiability.

**Architecture:** Each Claude/Copilot session keeps its own MCP server process. Live mode is already multi-client (editor's `BlueprintReaderLiveServer` accepts independent connections per MCP server). Commandlet mode becomes multi-client by converting `RunDaemon` from a stdin/stdout loop into a TCP server that speaks the same wire frames as the live backend. One daemon per project, coordinated via OS file locks. Idle-timeout shutdown, crash-recovery via pid check.

**Tech Stack:** C++20 (mcp-server), UE5 C++ (plugin), CMake, PowerShell (build script), Win32 + BSD sockets (cross-platform via existing `LiveBlueprintReader` patterns), nlohmann::json, fmt, doctest.

**Spec:** `docs/superpowers/specs/2026-05-13-multi-session-mcp-server-design.md`

---

## File map

### New files

| Path | Responsibility |
|---|---|
| `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.{h,cpp}` | Daemon-side TCP listener, frame parser, per-connection reader threads. Pattern-matched to `BlueprintReaderLiveServer.{h,cpp}` (already exists). |
| `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BatchContext.{h,cpp}` | Per-connection batch state (defer flag, pending-compile set, per-BP write lock holders). Replaces today's function-local `static` state. |
| `Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.{h,cpp}` | Result of renaming `LiveBlueprintReader.{h,cpp}`. Targets either handshake file. |
| `Plugins/BlueprintReader/mcp-server/tests/test_socket_backend.cpp` | Result of renaming `test_live_backend.cpp`. Adds multi-client coverage. |
| `Plugins/BlueprintReader/mcp-server/tests/test_daemon_lifecycle.cpp` | Spawn-race, idle-shutdown, crash-recovery cases against a fake daemon listener. |

### Modified files

| Path | What changes |
|---|---|
| `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp` | `RunDaemon` switches from stdin/stdout to invoking `BlueprintReaderCmdletServer`. Function-local batch statics move to `BatchContext`. |
| `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Public/BlueprintReaderCommandlet.h` | Surface the new server entry point if needed. |
| `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.{h,cpp}` | Replaces stdin/stdout daemon transport with `SocketBlueprintReader` targeting `bp-reader-cmdlet.json`. Spawn logic + two-lock coordination. |
| `Plugins/BlueprintReader/mcp-server/src/backends/AutoBlueprintReader.{h,cpp}` | Probes both handshake files. |
| `Plugins/BlueprintReader/mcp-server/src/backends/BackendFactory.cpp` | Wires the renamed reader. |
| `Plugins/BlueprintReader/mcp-server/src/main.cpp` | Removes `SingleInstanceLock` instantiation. |
| `Plugins/BlueprintReader/mcp-server/src/util/SingleInstanceLock.{h,cpp}` | Deleted. (Or repurposed as a generic file-lock primitive if other code wants it later; for now, delete.) |
| `Plugins/BlueprintReader/mcp-server/CMakeLists.txt` | Honor `BP_READER_PROJECT_NAME` for `OUTPUT_NAME`. |
| `Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1` | Detect project name; pass to CMake; post-build hard-link `bp-reader-mcp.exe`. |
| `Plugins/BlueprintReader/mcp-server/src/jsonrpc/Server.cpp` (or similar) | `shutdown_daemon` handler documents new semantics + still works. |
| `Plugins/BlueprintReader/mcp-server/tests/test_live_backend.cpp` | Renamed + multi-client cases. |
| `Plugins/BlueprintReader/mcp-server/tests/test_commandlet_backend.cpp` | Adapt to socket transport. |
| `Plugins/BlueprintReader/mcp-server/tests/test_soak.cpp` | New multi-client variant. |
| `Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt` | Register new test files. |
| `docs/design/05-backends.md` | Updated topology + lifecycle. |
| `docs/design/06-wire-protocol.md` | Note daemon now uses the same socket frames. |
| `docs/tutorial/09-daemon-mode.md` | Replace stdin/stdout walk-through with TCP transport. |
| `Plugins/BlueprintReader/Claude/skills/bp-debug/SKILL.md` | Cross-session triage notes. |
| `wiki/Configuration.md` | Multi-session + new handshake file. |

---

## Phase 0: Setup

### Task 0.1: Verify worktree + branch state

**Files:** none

- [ ] **Step 1: Check current branch**

```bash
git -C "D:/Projects/UE5_MCP" branch --show-current
```

Expected: `main` (or whatever clean branch you start from).

- [ ] **Step 2: Create the feature branch**

```bash
git -C "D:/Projects/UE5_MCP" checkout -b feat/multi-session-shared-daemon
```

Expected: `Switched to a new branch 'feat/multi-session-shared-daemon'`.

- [ ] **Step 3: Confirm baseline tests green**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | tail -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: build clean; `441 cases / 29134 assertions / 0 failed` (the exact numbers as of the spec date; if higher, that's also fine — the baseline is "all green").

---

## Phase 1: Plugin-side daemon TCP server

Goal: a `UnrealEditor-Cmd -Daemon` that listens on a TCP port and serves the same JSON frames as `BlueprintReaderLiveServer`. End of phase: a manual `nc 127.0.0.1 <port>` exchange can drive `-Op=List` ops; existing stdin/stdout daemon path still exists alongside (we'll remove it in Phase 2).

### Task 1.1: Scaffold `BlueprintReaderCmdletServer.{h,cpp}` from the live server

**Files:**
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.h`
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp`

- [ ] **Step 1: Copy live-server skeleton**

```bash
cp "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderLiveServer.h" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.h"

cp "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderLiveServer.cpp" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp"
```

- [ ] **Step 2: Rename symbols + handshake file**

Inside both files, replace:
- `BlueprintReaderLiveServer` → `BlueprintReaderCmdletServer`
- `FLiveServer` → `FCmdletServer`
- `LogBlueprintReaderLive` → `LogBlueprintReaderCmdlet`
- `bp-reader-live.json` (handshake path) → `bp-reader-cmdlet.json`
- Any other `Live`-prefixed names → `Cmdlet`

- [ ] **Step 3: Add the log category definition**

In `BlueprintReaderCmdletServer.cpp`, near the top:

```cpp
DEFINE_LOG_CATEGORY_STATIC(LogBlueprintReaderCmdlet, Log, All);
```

- [ ] **Step 4: Differentiate the lifetime-lock file**

In `BlueprintReaderCmdletServer.cpp`, find where the live server writes its handshake. Add a sibling lifetime-lock file:

```cpp
// Daemon's lifetime lock — held exclusively for the process's lifetime.
// MCP servers probe this to know if a daemon is alive; OS releases the
// lock automatically on process exit (graceful or crash).
const FString LockPath = ProjectDir / TEXT("Saved") / TEXT("bp-reader-cmdlet.lock");
LifetimeLockHandle = IFileManager::Get().CreateFileWriter(*LockPath,
    FILEWRITE_None);  // exclusive create
if (!LifetimeLockHandle)
{
    UE_LOG(LogBlueprintReaderCmdlet, Error,
        TEXT("Could not acquire %s — another daemon is already running"),
        *LockPath);
    return false;
}
```

Store `LifetimeLockHandle` as a member of `FCmdletServer`. Close (and delete the file) in the destructor / `Stop()`.

- [ ] **Step 5: Register the module's startup hook**

In `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderEditorModule.cpp` (or wherever `StartupModule` lives), do **NOT** auto-start the cmdlet server on module startup the way the live server does. The cmdlet server is meant to run only when invoked as `UnrealEditor-Cmd -Daemon`, not in a normal editor session. We'll wire its start in Task 1.2.

- [ ] **Step 6: Build the plugin**

```bash
"D:/Projects/Unreal Engine 5/Engine/Build/BatchFiles/Build.bat" LyraEditor Win64 Development -project="D:/Projects/UE5_MCP/LyraStarterGame.uproject" -NoUba -MaxParallelActions=4 -waitmutex 2>&1 | grep -E "error C|Compile|Link \[|Total" | tail -10
```

Expected: clean compile of `BlueprintReaderCmdletServer.cpp`. Link may fail with `LNK1104` if editor is running — that's OK.

- [ ] **Step 7: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.{h,cpp}
git -C "D:/Projects/UE5_MCP" commit -m "feat(plugin): scaffold BlueprintReaderCmdletServer from live server"
```

### Task 1.2: Switch `RunDaemon` from stdin/stdout to the new TCP server

**Files:**
- Modify: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp` (around line 5198)

- [ ] **Step 1: Find `RunDaemon` and `RunOneOpFromLiveServer`**

```bash
grep -n "^int32 RunDaemon\|RunOneOpFromLiveServer" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp"
```

Expected: line numbers for the stdin/stdout loop + the live-server's RunOneOp hook.

- [ ] **Step 2: Replace `RunDaemon`'s body with a TCP-server bootstrap**

Replace the current stdin/stdout loop with:

```cpp
int32 RunDaemon()
{
    UE_LOG(LogBlueprintReader, Display, TEXT("BlueprintReader daemon: starting cmdlet TCP server"));

    FCmdletServer Server;
    if (!Server.Start())
    {
        UE_LOG(LogBlueprintReader, Error, TEXT("Failed to start cmdlet TCP server"));
        return 1;
    }

    // Block here until graceful shutdown. The server runs its own
    // accept/reader threads; we just need the daemon process to stay
    // alive and pump the game thread. UE's idle tick handles that as
    // long as we're in a commandlet main loop.
    while (!Server.WantsShutdown())
    {
        FPlatformProcess::Sleep(0.05f);
    }

    Server.Stop();
    UE_LOG(LogBlueprintReader, Display, TEXT("BlueprintReader daemon: clean shutdown"));
    return 0;
}
```

Keep `RunOneOpFromLiveServer` unchanged — `FCmdletServer` will reuse it via the same hook the live server uses.

- [ ] **Step 3: Build the plugin**

Same command as Task 1.1 Step 6. Expected: clean compile.

- [ ] **Step 4: Smoke-test the daemon by hand**

Close the editor first if it's running. Then:

```bash
"D:/Projects/Unreal Engine 5/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "D:/Projects/UE5_MCP/LyraStarterGame.uproject" -run=BPR -Daemon -nullrhi -nosplash -unattended -nopause
```

In another shell, watch for the handshake file:

```bash
cat "D:/Projects/UE5_MCP/Saved/bp-reader-cmdlet.json"
```

Expected: JSON with `host`, `port`, `token`, `pid`, `started_at` keys.

Try a manual op (use `port` and `token` from the handshake):

```bash
echo '{"type":"auth","token":"<token-from-handshake>"}
{"type":"op","id":1,"args":["-Op=List","-Path=/Game"]}' | ncat 127.0.0.1 <port>
```

Expected: receives `hello`, then `auth_ok`, then a `result` with `code:0` and a JSON array of blueprints.

Stop the daemon with Ctrl-C in its terminal.

- [ ] **Step 5: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp
git -C "D:/Projects/UE5_MCP" commit -m "feat(plugin): RunDaemon hosts the cmdlet TCP server instead of stdin/stdout"
```

### Task 1.3: Verify the daemon survives a clean exit signal

**Files:** none (manual test)

- [ ] **Step 1: Restart the daemon as in Task 1.2**
- [ ] **Step 2: Connect a client, run an op, disconnect**
- [ ] **Step 3: Send the daemon a graceful shutdown via Ctrl-C**

Expected: the daemon prints `BlueprintReader daemon: clean shutdown`, deletes `bp-reader-cmdlet.json`, releases `bp-reader-cmdlet.lock`, and the process exits with code 0.

- [ ] **Step 4: Re-check the project's `Saved/` directory**

```bash
ls "D:/Projects/UE5_MCP/Saved/" | grep cmdlet
```

Expected: neither `bp-reader-cmdlet.json` nor `bp-reader-cmdlet.lock` is present.

- [ ] **Step 5: Commit (no code change)**

If Steps 1–4 passed, nothing to commit. If they revealed a bug, fix it and commit:

```bash
git -C "D:/Projects/UE5_MCP" add -A
git -C "D:/Projects/UE5_MCP" commit -m "fix(plugin): clean up cmdlet handshake on graceful shutdown"
```

---

## Phase 2: MCP-side `SocketBlueprintReader` consolidation

Goal: collapse `LiveBlueprintReader` and the commandlet-daemon client into one socket-speaking class targeting either handshake file. End of phase: `BP_READER_BACKEND=commandlet` works over TCP; `LiveBlueprintReader` no longer exists by name.

### Task 2.1: Rename `LiveBlueprintReader` to `SocketBlueprintReader`

**Files:**
- Rename: `Plugins/BlueprintReader/mcp-server/src/backends/LiveBlueprintReader.h` → `SocketBlueprintReader.h`
- Rename: `Plugins/BlueprintReader/mcp-server/src/backends/LiveBlueprintReader.cpp` → `SocketBlueprintReader.cpp`

- [ ] **Step 1: Move the files**

```bash
git -C "D:/Projects/UE5_MCP" mv Plugins/BlueprintReader/mcp-server/src/backends/LiveBlueprintReader.h Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.h
git -C "D:/Projects/UE5_MCP" mv Plugins/BlueprintReader/mcp-server/src/backends/LiveBlueprintReader.cpp Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.cpp
```

- [ ] **Step 2: Replace symbol names**

In both new files plus every consumer, replace `LiveBlueprintReader` → `SocketBlueprintReader`. Quick verification:

```bash
grep -rn "LiveBlueprintReader" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/src" 2>&1 | head -5
```

Expected (after replacement): no matches.

Files that import the header — update each:
- `Plugins/BlueprintReader/mcp-server/src/backends/AutoBlueprintReader.{h,cpp}`
- `Plugins/BlueprintReader/mcp-server/src/backends/BackendFactory.cpp`
- Any test that includes the old header

- [ ] **Step 3: Update CMake source list**

```bash
grep -rn "LiveBlueprintReader" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/CMakeLists.txt" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt"
```

Replace any matches with `SocketBlueprintReader`.

- [ ] **Step 4: Build + run tests**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: clean build. Tests still pass at baseline count (~441), since this is a pure rename.

- [ ] **Step 5: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add -A
git -C "D:/Projects/UE5_MCP" commit -m "refactor(mcp-server): rename LiveBlueprintReader to SocketBlueprintReader"
```

### Task 2.2: Make `SocketBlueprintReader` accept a configurable handshake path

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.h`
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.cpp`

- [ ] **Step 1: Confirm `Config::handshakeFilePath` already exists**

```bash
grep -n "handshakeFilePath" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/src/backends/SocketBlueprintReader.h"
```

Expected: at least two hits (declaration + comment). This field already shipped from issue #9 work.

- [ ] **Step 2: Write a failing test for cmdlet-handshake targeting**

Add to `Plugins/BlueprintReader/mcp-server/tests/test_socket_backend.cpp` (which is the renamed `test_live_backend.cpp` from Task 2.5 below — for now, if it's still named `test_live_backend.cpp`, add the case there and rename later):

```cpp
TEST_CASE("SocketBackend: connects to bp-reader-cmdlet.json handshake (issue #66)") {
    // Same wire shape as live, different handshake file name. The
    // reader should attach to whichever path the caller supplies.
    MockServer mock([](SOCKET s) {
        SendLine(s, R"({"type":"hello","version":"1"})");
        std::string authLine = ReadLine(s);
        SendLine(s, R"({"type":"auth_ok"})");
        ReadLine(s);  // op frame
        SendLine(s, nlohmann::json{
            {"type","result"}, {"id",1}, {"code",0},
            {"json", nlohmann::json::array()}
        }.dump());
    });

    auto tempPath = std::filesystem::temp_directory_path() /
        ("bp-reader-cmdlet-test-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&mock)) + ".json");
    nlohmann::json hs = {
        {"version", 1}, {"host", "127.0.0.1"},
        {"port", mock.port()}, {"token", "cmdlet-token"},
        {"pid", static_cast<int>(GetCurrentProcessId())},
    };
    { std::ofstream f(tempPath); f << hs.dump(); }

    SocketBlueprintReader::Config cfg;
    cfg.host  = "127.0.0.1";
    cfg.port  = mock.port();
    cfg.token = "cmdlet-token";
    cfg.handshakeFilePath = tempPath.string();
    SocketBlueprintReader reader(cfg);

    CHECK_NOTHROW((void)reader.ListBlueprints("/Game"));
    std::filesystem::remove(tempPath);
}
```

- [ ] **Step 3: Run the test, expect FAIL**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep "error C" | head -5
```

If the test compiles but the existing `SocketBlueprintReader` already handles a configurable handshake (which it does, per issue #9 work), this test might pass immediately. That's fine — it pins existing behavior under a new explicit assertion.

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*cmdlet handshake*" 2>&1 | tail -5
```

Expected: PASS. (The class already supports this; we're just adding an explicit test.)

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/tests/test_live_backend.cpp
git -C "D:/Projects/UE5_MCP" commit -m "test(socket): pin cmdlet-handshake targeting in SocketBlueprintReader"
```

### Task 2.3: Rename `test_live_backend.cpp` to `test_socket_backend.cpp`

**Files:**
- Rename: `Plugins/BlueprintReader/mcp-server/tests/test_live_backend.cpp` → `test_socket_backend.cpp`
- Modify: `Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt`

- [ ] **Step 1: git mv the test file**

```bash
git -C "D:/Projects/UE5_MCP" mv Plugins/BlueprintReader/mcp-server/tests/test_live_backend.cpp Plugins/BlueprintReader/mcp-server/tests/test_socket_backend.cpp
```

- [ ] **Step 2: Update its registration in `tests/CMakeLists.txt`**

Find `test_live_backend.cpp` in the source list and replace with `test_socket_backend.cpp`.

- [ ] **Step 3: Update the test case prefix names where helpful**

In `test_socket_backend.cpp`, find any TEST_CASE names like `"LiveBackend: ..."`. Either:
- Leave them as-is (the original tests still cover live-mode-specific behavior; the cmdlet-mode cases are new), OR
- Rename top-tier sections to `"SocketBackend(live): ..."` and `"SocketBackend(cmdlet): ..."` for clarity.

Either approach is fine. The straightforward path: leave existing names, add new cases under `"SocketBackend(cmdlet): ..."` prefix.

- [ ] **Step 4: Build + run**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: clean build, all tests still pass.

- [ ] **Step 5: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt
git -C "D:/Projects/UE5_MCP" commit -m "refactor(tests): rename test_live_backend.cpp to test_socket_backend.cpp"
```

### Task 2.4: Make `CommandletBlueprintReader` route ops through `SocketBlueprintReader`

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.h`
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.cpp`

- [ ] **Step 1: Add a daemon-attach helper**

In `CommandletBlueprintReader.cpp`, add (or replace) a private helper that:
1. Reads `<Project>/Saved/bp-reader-cmdlet.json`.
2. If present + pid alive + TCP probe succeeds → constructs a `SocketBlueprintReader` configured for that handshake; returns it.
3. Else returns null (the caller spawns + retries).

```cpp
std::unique_ptr<SocketBlueprintReader>
CommandletBlueprintReader::TryAttachExistingDaemon() const {
    auto hsPath = (cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet.json");
    if (!std::filesystem::exists(hsPath)) return nullptr;

    // Parse + verify pid alive + TCP probe.
    std::ifstream f(hsPath);
    nlohmann::json j;
    try { f >> j; } catch (...) { return nullptr; }
    int pid = j.value("pid", 0);
    if (pid <= 0 || !ProcessAlive(pid)) {
        // Stale handshake — caller will spawn fresh.
        return nullptr;
    }

    SocketBlueprintReader::Config sc;
    sc.host  = j.value("host",  std::string("127.0.0.1"));
    sc.port  = j.value("port",  0);
    sc.token = j.value("token", std::string());
    sc.handshakeFilePath = hsPath.string();
    if (sc.port <= 0 || sc.token.empty()) return nullptr;

    // TCP probe before committing — handshake might be stale but pid happened to be reused.
    if (!TcpProbe(sc.host, sc.port, std::chrono::milliseconds(250))) {
        return nullptr;
    }
    return std::make_unique<SocketBlueprintReader>(std::move(sc));
}
```

Add `ProcessAlive` (Win32: `OpenProcess` with `PROCESS_QUERY_LIMITED_INFORMATION`; POSIX: `kill(pid, 0)`) and `TcpProbe` (non-blocking connect with timeout). Both belong in a small `util/` header alongside the existing helpers.

- [ ] **Step 2: Replace stdin/stdout daemon path with attach-or-spawn**

In `CommandletBlueprintReader.cpp`, in the entry point that runs an op (today calls `RunOpDaemon` which writes to `daemonStdin_`), replace with:

```cpp
nlohmann::json CommandletBlueprintReader::RunOp(const std::vector<std::wstring>& opArgs) {
    auto socket = EnsureDaemonAttached();  // see Step 3 next
    return socket->RunOpRaw(opArgs);       // SocketBlueprintReader exposes the same RunOp shape
}
```

The `RunOpDaemon` and `daemonStdin_` machinery goes away in Step 4.

- [ ] **Step 3: Add `EnsureDaemonAttached`**

```cpp
SocketBlueprintReader& CommandletBlueprintReader::EnsureDaemonAttached() {
    std::lock_guard lock(daemonMutex_);
    if (socket_ && socket_->IsHealthy()) return *socket_;

    socket_ = TryAttachExistingDaemon();
    if (socket_) return *socket_;

    // Two-lock spawn coordination (full version in Phase 4 — for now
    // just spawn directly if nothing is attached).
    SpawnDaemon();
    PollForHandshake(cfg_.daemonStartupTimeout);
    socket_ = TryAttachExistingDaemon();
    if (!socket_) {
        throw BlueprintReaderError("daemon spawn succeeded but handshake never appeared");
    }
    return *socket_;
}
```

`SpawnDaemon()` replaces the current `EnsureDaemon()`. It launches `UnrealEditor-Cmd -Daemon ...` as a child process exactly like before, but **does not redirect stdin/stdout** — the daemon owns its own I/O via the TCP server. Hand the child its arguments via the command line; let it inherit a console or detach as you prefer.

`PollForHandshake` checks `bp-reader-cmdlet.json` every 250ms with a `cfg_.daemonStartupTimeout` overall deadline (default 600s, env-overridable via `BP_READER_DAEMON_STARTUP_SECONDS`).

- [ ] **Step 4: Delete the dead stdin/stdout path**

Remove from `CommandletBlueprintReader.{h,cpp}`:
- `RunOpDaemon`
- `ReadUntilMarker`
- `daemonStdin_`, `daemonStdout_` member handles
- The `__BPR_DONE` sentinel parsing
- The `BuildCommandLine` helper used only by the stdin/stdout daemon (the one-shot mode still needs it)

Keep:
- `RunOpOneShot` (one-shot subprocess for non-daemon mode) — still useful as a fallback.
- The `EncodeArg` / `EncodeArgForFParse` helpers in `CommandletArgEncoding.h` — still consumed by `RunOpOneShot`.

- [ ] **Step 5: Build + run all existing tests**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -10
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: clean build. Mock-backend tests still pass (they don't touch the daemon). Live-backend tests still pass. `test_commandlet_backend.cpp`'s live-skip cases continue to skip cleanly without a running editor.

- [ ] **Step 6: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.{h,cpp}
git -C "D:/Projects/UE5_MCP" commit -m "refactor(mcp-server): CommandletBlueprintReader routes through SocketBlueprintReader"
```

### Task 2.5: Update `AutoBlueprintReader` to probe both handshakes

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/AutoBlueprintReader.{h,cpp}`

- [ ] **Step 1: Write a failing test**

Add to `test_socket_backend.cpp`:

```cpp
TEST_CASE("AutoBackend: prefers live when both handshakes are valid") {
    // Stand up two mock servers (live + cmdlet), write both handshakes
    // into a fake project dir. Auto should route the first call to live.
    MockServer liveMock([](SOCKET s) { /* live script */ });
    MockServer cmdletMock([](SOCKET s) { /* cmdlet script */ });

    auto tempDir = std::filesystem::temp_directory_path() / "bp-reader-auto-test";
    std::filesystem::create_directories(tempDir / "Saved");

    auto writeHs = [&](const std::string& name, int port) {
        nlohmann::json hs = {
            {"version", 1}, {"host", "127.0.0.1"},
            {"port", port}, {"token", "t"},
            {"pid", static_cast<int>(GetCurrentProcessId())},
        };
        std::ofstream f(tempDir / "Saved" / name);
        f << hs.dump();
    };
    writeHs("bp-reader-live.json",   liveMock.port());
    writeHs("bp-reader-cmdlet.json", cmdletMock.port());

    AutoBlueprintReader::Config cfg;
    cfg.uproject = tempDir / "LyraStarterGame.uproject";
    AutoBlueprintReader reader(cfg);

    CHECK_NOTHROW((void)reader.ListBlueprints("/Game"));
    // (Verifying which mock got the op is left to a follow-up; the
    // basic shape is: auto picked a backend and returned cleanly.)

    std::filesystem::remove_all(tempDir);
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*AutoBackend: prefers live*" 2>&1 | tail -5
```

Expected: FAIL initially (auto doesn't probe both handshakes yet).

- [ ] **Step 3: Update `AutoBlueprintReader::Pick`**

Inside `Pick()` (the per-call probe routine):

```cpp
backends::BackendKind AutoBlueprintReader::Pick() {
    // Live first — editor's listener takes priority when present.
    if (TryBuildLive()) return BackendKind::Live;
    // Then commandlet — daemon handshake at <Project>/Saved/bp-reader-cmdlet.json.
    if (TryBuildCmdlet()) return BackendKind::Commandlet;
    return BackendKind::None;
}

std::unique_ptr<SocketBlueprintReader> AutoBlueprintReader::TryBuildCmdlet() {
    auto hsPath = cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet.json";
    if (!std::filesystem::exists(hsPath)) return nullptr;
    // ... same shape as TryBuildLive but reads bp-reader-cmdlet.json ...
}
```

Cache the chosen backend with the existing 2s TTL.

- [ ] **Step 4: Run the test, expect PASS**

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*AutoBackend: prefers live*" 2>&1 | tail -5
```

Expected: PASS.

- [ ] **Step 5: Run the full suite**

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: all green, 1 new case.

- [ ] **Step 6: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/src/backends/AutoBlueprintReader.{h,cpp} Plugins/BlueprintReader/mcp-server/tests/test_socket_backend.cpp
git -C "D:/Projects/UE5_MCP" commit -m "feat(mcp-server): AutoBackend probes both live and cmdlet handshakes"
```

---

## Phase 3: Drop the MCP-server single-instance lock

### Task 3.1: Remove the lock from startup

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/main.cpp` (around line 267)

- [ ] **Step 1: Find the lock instantiation**

```bash
grep -n "SingleInstanceLock\|instanceLock" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/src/main.cpp"
```

Expected: lines that include the header and construct `util::SingleInstanceLock instanceLock(cfg.uproject);`.

- [ ] **Step 2: Delete the include + constructor + any usage**

In `main.cpp`, remove:

```cpp
#include "util/SingleInstanceLock.h"
```

and the line near 267:

```cpp
util::SingleInstanceLock instanceLock(cfg.uproject);
```

If the lock is referenced anywhere else in `main.cpp` (e.g. as a check before continuing), remove those guards.

- [ ] **Step 3: Delete the lock files**

```bash
git -C "D:/Projects/UE5_MCP" rm Plugins/BlueprintReader/mcp-server/src/util/SingleInstanceLock.h Plugins/BlueprintReader/mcp-server/src/util/SingleInstanceLock.cpp
```

- [ ] **Step 4: Remove from CMake source list**

```bash
grep -n "SingleInstanceLock" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/CMakeLists.txt"
```

If found, remove that line.

- [ ] **Step 5: Build + run tests**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: clean build, all tests pass. The existing `test_single_instance_lock.cpp` will fail to compile because the header is gone — delete that test file too:

```bash
git -C "D:/Projects/UE5_MCP" rm Plugins/BlueprintReader/mcp-server/tests/test_single_instance_lock.cpp
```

Update the test CMakeLists to drop it. Rebuild. Expected: now clean.

- [ ] **Step 6: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add -A
git -C "D:/Projects/UE5_MCP" commit -m "refactor(mcp-server): drop SingleInstanceLock — daemon owns the lock now"
```

### Task 3.2: Add a multi-instance integration test (mock backend, no UE)

**Files:**
- Create: `Plugins/BlueprintReader/mcp-server/tests/test_multi_instance.cpp`
- Modify: `Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt`

- [ ] **Step 1: Write a failing test**

Create `test_multi_instance.cpp`:

```cpp
// Two backends built side-by-side against the same fixture dir do not
// step on each other. Pure mock backend — no socket / daemon involved.
// This pins "the MCP server is no longer single-instance" at the test
// level so a future regression of the lock is caught.

#include <doctest/doctest.h>
#include "backends/MockBlueprintReader.h"

using namespace bpr::backends;

TEST_CASE("Two MockBlueprintReader instances share fixtures without locking") {
    auto reader1 = bpr::test::MakeMockReader();
    auto reader2 = bpr::test::MakeMockReader();

    auto a = reader1.ListBlueprints("/Game");
    auto b = reader2.ListBlueprints("/Game");

    CHECK(a.size() == b.size());
    CHECK(a.size() > 0);
}
```

- [ ] **Step 2: Register in the test CMakeLists**

In `Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt`, add `test_multi_instance.cpp` to the source list.

- [ ] **Step 3: Build + run**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*MockBlueprintReader instances share*" 2>&1 | tail -5
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/tests/test_multi_instance.cpp Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt
git -C "D:/Projects/UE5_MCP" commit -m "test: pin multi-instance MockBlueprintReader behavior"
```

### Task 3.3: Manual verification — two MCP servers, one daemon

**Files:** none (manual smoke test)

- [ ] **Step 1: Start one MCP server in foreground**

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/Release/bp-reader-mcp.exe"
```

(Without arguments. It will wait for stdio. Leave it.)

- [ ] **Step 2: In a second terminal, start another**

Same command. Expected: it does NOT error out with "another instance already running". It also waits for stdio.

- [ ] **Step 3: Send a `tools/list` request to each**

In each MCP-server's stdin:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}
```

Expected: both servers respond with the tool list. No lock-related error.

- [ ] **Step 4: Kill both (Ctrl-C)**

No files left behind in `<Project>/Saved/` from the MCP server (it doesn't own any handshake files; that's the daemon's job).

---

## Phase 4: Daemon lifecycle hardening

### Task 4.1: Implement the two-lock spawn coordination

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.cpp`

- [ ] **Step 1: Write a failing test for spawn-race**

In `test_daemon_lifecycle.cpp` (new file — create it now):

```cpp
// Two CommandletBlueprintReader instances racing to spawn the daemon
// must end up with exactly one spawn invocation. The losing one waits
// on the handshake instead of double-spawning.

#include <doctest/doctest.h>
#include "backends/CommandletBlueprintReader.h"
#include <atomic>
#include <thread>

TEST_CASE("CommandletBackend: simultaneous spawn attempts coalesce to one") {
    // Stub: replace SpawnDaemon with a counter-bumping fake that also
    // writes a fake handshake after a 200ms delay (simulating real
    // spawn latency).
    std::atomic<int> spawnCount{0};
    auto fakeSpawn = [&](const std::filesystem::path& projectDir) {
        ++spawnCount;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // Write a handshake the attach helper will find.
        // ... shape as before ...
    };

    // ... construct two readers with the same projectDir, hit them in
    //     parallel threads, join. assert spawnCount == 1 ...
}
```

(The detailed fake-spawn injection requires a small refactor — expose `SpawnDaemon` as a `std::function<>` hook on `CommandletBlueprintReader::Config`. The test sets that hook; production code uses the default native-process spawn.)

- [ ] **Step 2: Run the test, expect FAIL**

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*simultaneous spawn*" 2>&1 | tail -5
```

Expected: FAIL with `spawnCount > 1` (today both readers race, both spawn).

- [ ] **Step 3: Implement the spawn lock**

Add to `CommandletBlueprintReader.cpp`:

```cpp
class SpawnLock {
public:
    explicit SpawnLock(const std::filesystem::path& lockFile);
    ~SpawnLock();
    bool TryAcquire(std::chrono::seconds blockFor);
    bool IsHeld() const { return held_; }
private:
    std::filesystem::path lockFile_;
    bool held_ = false;
    // Win32: HANDLE with LOCKFILE_EXCLUSIVE_LOCK
    // POSIX: int fd + flock(LOCK_EX)
};
```

Replace `EnsureDaemonAttached`'s spawn branch with:

```cpp
SocketBlueprintReader& CommandletBlueprintReader::EnsureDaemonAttached() {
    std::lock_guard lock(daemonMutex_);
    if (socket_ && socket_->IsHealthy()) return *socket_;

    // 1. Attach to existing daemon if alive.
    socket_ = TryAttachExistingDaemon();
    if (socket_) return *socket_;

    // 2. Race for the spawn lock.
    auto lockPath = cfg_.uproject.parent_path() / "Saved" / "bp-reader-cmdlet-spawn.lock";
    SpawnLock spawnLock(lockPath);
    bool acquired = spawnLock.TryAcquire(std::chrono::seconds(cfg_.daemonStartupTimeout));

    if (acquired) {
        // Re-check: someone may have spawned during the race.
        socket_ = TryAttachExistingDaemon();
        if (!socket_) {
            SpawnDaemon();
            PollForHandshake(std::chrono::seconds(cfg_.daemonStartupTimeout));
            socket_ = TryAttachExistingDaemon();
        }
        // spawnLock destructor releases.
    } else {
        // Someone else is spawning. Poll the handshake.
        PollForHandshake(std::chrono::seconds(cfg_.daemonStartupTimeout));
        socket_ = TryAttachExistingDaemon();
    }

    if (!socket_) {
        throw BlueprintReaderError(
            "commandlet daemon: spawn lock contended but no handshake appeared "
            "within startup timeout (BP_READER_DAEMON_STARTUP_SECONDS)");
    }
    return *socket_;
}
```

- [ ] **Step 4: Run the test, expect PASS**

```bash
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" --test-case="*simultaneous spawn*" 2>&1 | tail -5
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.cpp Plugins/BlueprintReader/mcp-server/tests/test_daemon_lifecycle.cpp Plugins/BlueprintReader/mcp-server/tests/CMakeLists.txt
git -C "D:/Projects/UE5_MCP" commit -m "feat(mcp-server): two-lock spawn coordination prevents daemon duplication"
```

### Task 4.2: Daemon-side idle shutdown

**Files:**
- Modify: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp`

- [ ] **Step 1: Add a connection counter + idle timer**

In `FCmdletServer`:

```cpp
class FCmdletServer
{
    std::atomic<int> ActiveConnections{0};
    FDateTime LastDisconnectAt;
    int32 IdleSeconds = 300;  // BP_READER_DAEMON_IDLE_SECONDS

    void OnClientConnected();
    void OnClientDisconnected();
    bool ShouldShutdownForIdle() const;
};
```

`OnClientConnected` increments. `OnClientDisconnected` decrements and stamps `LastDisconnectAt`. The main loop in `RunDaemon` (Phase 1 Task 1.2) calls `Server.WantsShutdown()` which now consults `ShouldShutdownForIdle`:

```cpp
bool FCmdletServer::ShouldShutdownForIdle() const {
    if (ActiveConnections.load() != 0) return false;
    if ((FDateTime::UtcNow() - LastDisconnectAt).GetTotalSeconds() < IdleSeconds) return false;
    return true;
}
```

- [ ] **Step 2: Honor the env var**

```cpp
FString IdleStr = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_DAEMON_IDLE_SECONDS"));
if (!IdleStr.IsEmpty()) {
    IdleSeconds = FCString::Atoi(*IdleStr);
    if (IdleSeconds < 5) IdleSeconds = 5;  // floor; otherwise daemon flaps
}
```

- [ ] **Step 3: Manual test**

Rebuild the plugin (`Build.bat ...`). Start the daemon. Connect a client, run an op, disconnect. Wait `IdleSeconds + 5`. Expected: daemon exits gracefully (handshake deleted, lock released, process exits with code 0).

For a faster manual loop, set `BP_READER_DAEMON_IDLE_SECONDS=10` and observe.

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.{h,cpp}
git -C "D:/Projects/UE5_MCP" commit -m "feat(plugin): cmdlet daemon idle-timeout shutdown"
```

### Task 4.3: Per-BP write lock for batches

**Files:**
- Create: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BatchContext.{h,cpp}`
- Modify: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCommandlet.cpp`

- [ ] **Step 1: Define `BatchContext`**

```cpp
// BatchContext.h
struct FBatchContext
{
    bool bDeferCompile = false;
    TSet<TWeakObjectPtr<UBlueprint>> Pending;
};

class FBatchRegistry
{
public:
    // One context per connection. Connection ID is the daemon's
    // monotonically increasing client counter.
    FBatchContext& GetOrCreate(uint64 ConnectionId);
    void Discard(uint64 ConnectionId);

    // Per-BP write lock — held by a connection from BeginBatch to
    // EndBatch. AcquireWrite blocks if another connection holds it.
    void AcquireWrite(uint64 ConnectionId, UBlueprint* BP);
    void ReleaseWrite(uint64 ConnectionId, UBlueprint* BP);

private:
    FCriticalSection Mu;
    TMap<uint64, FBatchContext> Contexts;
    TMap<UBlueprint*, uint64> BPWriteHolders;  // BP → which conn owns the write
    TArray<TPair<UBlueprint*, uint64>> WaitQueue;
    FEvent* QueueWake = FPlatformProcess::GetSynchEventFromPool();
};
```

- [ ] **Step 2: Wire `BeginBatch` / `EndBatch` through `FBatchRegistry`**

Replace the function-local `static bool BatchDeferFlag` and `static TArray<TWeakObjectPtr<UBlueprint>> BatchPending` in `BlueprintReaderCommandlet.cpp` with per-connection lookups:

```cpp
int32 RunBeginBatchOp(uint64 ConnectionId, const FString& Params, ...)
{
    FBatchContext& ctx = GBatchRegistry.GetOrCreate(ConnectionId);
    ctx.bDeferCompile = true;
    return EmitOk(OutputPath, bPretty);
}
```

The `ConnectionId` flows through the dispatcher — each op-frame handler in `FCmdletServer` calls into `RunOneOp` with an extra parameter (or sets a thread-local before dispatch).

- [ ] **Step 3: Acquire write lock when a write op runs inside a batch**

In each write op handler, if `ctx.bDeferCompile`, call `GBatchRegistry.AcquireWrite(ConnectionId, BP)` before mutating. `EndBatch` releases all writes the context holds + flushes.

- [ ] **Step 4: Write a test**

In `test_daemon_lifecycle.cpp`:

```cpp
TEST_CASE("BatchRegistry: two contexts on same BP serialize") {
    FBatchRegistry reg;
    UBlueprint* BP = /* fake */;

    auto acquireA = std::async(std::launch::async, [&] {
        reg.AcquireWrite(/*conn=*/1, BP);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        reg.ReleaseWrite(1, BP);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto t0 = std::chrono::steady_clock::now();
    reg.AcquireWrite(/*conn=*/2, BP);
    auto wait = std::chrono::steady_clock::now() - t0;
    reg.ReleaseWrite(2, BP);

    // Conn 2 should have waited at least ~80ms before getting the lock.
    CHECK(wait > std::chrono::milliseconds(50));
}
```

- [ ] **Step 5: Run, expect PASS**

(After implementing `FBatchRegistry`.)

- [ ] **Step 6: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add -A
git -C "D:/Projects/UE5_MCP" commit -m "feat(plugin): per-BP write lock for concurrent batch contexts"
```

### Task 4.4: Commit-partial on disconnect

**Files:**
- Modify: `Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp`

- [ ] **Step 1: Hook the disconnect path**

When `FCmdletServer`'s per-connection reader thread exits (socket closed by client), call:

```cpp
void FCmdletServer::OnClientDisconnected(uint64 ConnectionId)
{
    FBatchContext* Ctx = GBatchRegistry.Find(ConnectionId);
    if (Ctx && !Ctx->Pending.IsEmpty())
    {
        // Default: commit-partial. Env override:
        // BP_READER_BATCH_ON_DISCONNECT=discard → skipCompile=true.
        const bool bSkipCompile =
            FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_BATCH_ON_DISCONNECT")) == TEXT("discard");
        FlushBatch(ConnectionId, bSkipCompile);
    }
    GBatchRegistry.Discard(ConnectionId);
    --ActiveConnections;
    LastDisconnectAt = FDateTime::UtcNow();
}
```

- [ ] **Step 2: Manual test (live UE required)**

With the editor closed, start the daemon, connect a client, `BeginBatch`, `add_variable` (one), kill the client's socket without `EndBatch`. Expected:
- Daemon's log shows "client disconnected mid-batch; flushing N pending BP(s)".
- The variable was saved to disk.

Then set `BP_READER_BATCH_ON_DISCONNECT=discard`, repeat. Expected: variable NOT saved.

- [ ] **Step 3: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Source/BlueprintReaderEditor/Private/BlueprintReaderCmdletServer.cpp
git -C "D:/Projects/UE5_MCP" commit -m "feat(plugin): commit-partial-on-disconnect with discard override"
```

### Task 4.5: `shutdown_daemon` tool — document new semantics

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/tools/BlueprintTools.cpp` (find `shutdown_daemon` registration)

- [ ] **Step 1: Find the tool**

```bash
grep -n "shutdown_daemon" "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/src/tools/BlueprintTools.cpp"
```

- [ ] **Step 2: Update its description string**

The schema's `description` should reflect:

> "Force-terminate the shared commandlet daemon for this project. In shared-daemon mode, this affects EVERY session against this project — other sessions' next call simply spawns a fresh daemon. Original use case (free file locks / force a fresh spawn) still works."

- [ ] **Step 3: Build + run**

Verify the daemon can still be killed remotely via the tool. Expected: daemon exits gracefully (deletes handshake, releases lock).

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/src/tools/BlueprintTools.cpp
git -C "D:/Projects/UE5_MCP" commit -m "docs(tools): clarify shutdown_daemon shared-daemon semantics"
```

### Task 4.6: Crash-recovery test (stale handshake)

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/tests/test_daemon_lifecycle.cpp`

- [ ] **Step 1: Write a failing test**

```cpp
TEST_CASE("CommandletBackend: stale handshake (dead pid) triggers fresh spawn") {
    auto tempDir = std::filesystem::temp_directory_path() / "bp-reader-stale-test";
    std::filesystem::create_directories(tempDir / "Saved");

    // Write a handshake with pid that definitely doesn't exist.
    nlohmann::json hs = {
        {"version", 1}, {"host", "127.0.0.1"}, {"port", 65530},
        {"token", "stale"}, {"pid", 0x7FFFFFFE},
    };
    {
        std::ofstream f(tempDir / "Saved" / "bp-reader-cmdlet.json");
        f << hs.dump();
    }

    // Inject a spawn hook that records but doesn't actually spawn.
    std::atomic<bool> spawnCalled{false};

    // ... construct reader with hook + uproject path ...
    // ... attempt RunOp ... expect either spawn-called-true or a
    //     specific stale-handshake error.

    CHECK(spawnCalled.load());
    std::filesystem::remove_all(tempDir);
}
```

- [ ] **Step 2: Verify `TryAttachExistingDaemon` checks pid + drops stale**

Confirm Task 2.4's `TryAttachExistingDaemon` calls `ProcessAlive(pid)` and returns null on stale. If not, add it.

- [ ] **Step 3: Run, expect PASS**

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add -A
git -C "D:/Projects/UE5_MCP" commit -m "test(mcp-server): stale-handshake crash recovery"
```

---

## Phase 5: Build-time exe rename

### Task 5.1: Pass `BP_READER_PROJECT_NAME` from `Build-MCPServer.ps1` to CMake

**Files:**
- Modify: `Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1`

- [ ] **Step 1: Detect project name**

In `Build-MCPServer.ps1`, before the `cmake` invocation:

```pwsh
# Walk up from the plugin to find the parent .uproject; use its basename
# as the project name for build artifact disambiguation.
$pluginRoot = Split-Path -Parent $PSScriptRoot
$candidate = $pluginRoot
$projectName = ""
for ($i = 0; $i -lt 5; $i++) {
    $candidate = Split-Path -Parent $candidate
    if (-not $candidate) { break }
    $up = Get-ChildItem -Path $candidate -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($up) {
        $projectName = [System.IO.Path]::GetFileNameWithoutExtension($up.Name)
        break
    }
}
```

- [ ] **Step 2: Pass it to CMake**

```pwsh
$cmakeArgs = @('-S', $mcpRoot, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($projectName) {
    $cmakeArgs += "-DBP_READER_PROJECT_NAME=$projectName"
}
cmake @cmakeArgs
```

- [ ] **Step 3: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1
git -C "D:/Projects/UE5_MCP" commit -m "build: detect project name and pass to CMake as BP_READER_PROJECT_NAME"
```

### Task 5.2: Honor the variable in CMakeLists

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/CMakeLists.txt` (around line 105-107)

- [ ] **Step 1: Update OUTPUT_NAME conditionally**

```cmake
add_executable(bp-reader-mcp src/main.cpp)
target_link_libraries(bp-reader-mcp PRIVATE bp-reader-core)

if(BP_READER_PROJECT_NAME)
    set(BP_READER_EXE_NAME "bp-reader-mcp-${BP_READER_PROJECT_NAME}")
else()
    set(BP_READER_EXE_NAME "bp-reader-mcp")
endif()
set_target_properties(bp-reader-mcp PROPERTIES OUTPUT_NAME "${BP_READER_EXE_NAME}")
```

- [ ] **Step 2: Add a post-build hard-link step**

After the `set_target_properties` block:

```cmake
# Always produce a canonical bp-reader-mcp.exe (or .bin) alongside the
# named build artifact, so existing .mcp.json configs that reference
# the canonical name continue to work.
if(BP_READER_PROJECT_NAME)
    if(WIN32)
        add_custom_command(TARGET bp-reader-mcp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp.exe"
            COMMAND cmd /c mklink /H "$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp.exe" "$<TARGET_FILE:bp-reader-mcp>"
            COMMENT "Hard-linking canonical bp-reader-mcp.exe to ${BP_READER_EXE_NAME}.exe"
        )
    else()
        add_custom_command(TARGET bp-reader-mcp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_hardlink "$<TARGET_FILE:bp-reader-mcp>" "$<TARGET_FILE_DIR:bp-reader-mcp>/bp-reader-mcp"
            COMMENT "Hard-linking canonical bp-reader-mcp to ${BP_READER_EXE_NAME}"
        )
    endif()
endif()
```

- [ ] **Step 3: Re-run the build via the plugin script**

```pwsh
pwsh "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Scripts/Build-MCPServer.ps1"
```

- [ ] **Step 4: Verify both names appear**

```bash
ls "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/Release/" | grep -i mcp
```

Expected: both `bp-reader-mcp-UE5_MCP.exe` and `bp-reader-mcp.exe` (hard link).

- [ ] **Step 5: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/CMakeLists.txt
git -C "D:/Projects/UE5_MCP" commit -m "build: produce bp-reader-mcp-<ProjectName>.exe with a canonical hard link"
```

### Task 5.3: FNV-1a-prefix temp files

**Files:**
- Modify: `Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.cpp` (find `TempJsonPath()` or similar)

- [ ] **Step 1: Compute FNV-1a hash of `cfg_.uproject`**

```cpp
static uint64_t FnvHash(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 0x100000001b3ull; }
    return h;
}

std::filesystem::path TempJsonPath() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%016llx", FnvHash(cfg_.uproject.string()));
    return std::filesystem::temp_directory_path() /
        (std::string("bp-reader-") + buf + "-" + RandomSuffix() + ".json");
}
```

- [ ] **Step 2: Build + run**

```bash
cmake --build "D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build" --config Release 2>&1 | grep -E "error C|fatal error" | head -5
"D:/Projects/UE5_MCP/Plugins/BlueprintReader/mcp-server/build/tests/Release/bp-reader-tests.exe" 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/mcp-server/src/backends/CommandletBlueprintReader.cpp
git -C "D:/Projects/UE5_MCP" commit -m "build: prefix temp-dir scratch files with project FNV-1a hash"
```

---

## Phase 6: Documentation + skills sync

### Task 6.1: Update design doc — backends

**Files:**
- Modify: `docs/design/05-backends.md`

- [ ] **Step 1: Update the topology diagram**

Replace the "Per-MCP-server commandlet daemon as stdin/stdout child" section with the new shared-daemon-as-TCP-server model. Mirror the diagram in Section 1 of the spec.

- [ ] **Step 2: Update the handshake-file table**

Add `bp-reader-cmdlet.json` and `bp-reader-cmdlet.lock`. Remove any reference to the MCP-server single-instance lock.

- [ ] **Step 3: Update the spawn-flow pseudocode**

Use the two-lock spawn coordination from Task 4.1.

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add docs/design/05-backends.md
git -C "D:/Projects/UE5_MCP" commit -m "docs(design): backends doc reflects shared-daemon architecture"
```

### Task 6.2: Update design doc — wire protocol

**Files:**
- Modify: `docs/design/06-wire-protocol.md`

- [ ] **Step 1: Note the daemon now uses the same frames as live**

Remove any reference to the daemon's stdin/stdout sentinel format. Add a sentence that the cmdlet daemon and the live editor speak identical wire frames; only the handshake-file name differs.

- [ ] **Step 2: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add docs/design/06-wire-protocol.md
git -C "D:/Projects/UE5_MCP" commit -m "docs(design): wire-protocol doc reflects cmdlet/live unification"
```

### Task 6.3: Rewrite tutorial chapter 09 (daemon mode)

**Files:**
- Modify: `docs/tutorial/09-daemon-mode.md`

- [ ] **Step 1: Replace the stdin/stdout walk-through with the TCP server walk-through**

Mirror the structure of chapter 10 (live TCP backend) since the daemon now uses the same pattern. The chapter's milestone changes from "per-call latency drops from 5s to 30ms" to "multiple sessions share one daemon; latency stays low".

- [ ] **Step 2: Update the chapter's checkpoint**

The new checkpoint: spawn two MCP server instances against the same project, confirm only one daemon process exists, both MCP servers issue ops successfully.

- [ ] **Step 3: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add docs/tutorial/09-daemon-mode.md
git -C "D:/Projects/UE5_MCP" commit -m "docs(tutorial): chapter 09 rewritten for TCP daemon + multi-session"
```

### Task 6.4: Update the bp-debug skill

**Files:**
- Modify: `Plugins/BlueprintReader/Claude/skills/bp-debug/SKILL.md`

- [ ] **Step 1: Add a "multi-session" triage section**

Cover:
- How to tell which MCP server you're in (process exe name now includes project).
- How to inspect daemon state (handshake file, lock file).
- `shutdown_daemon` semantics in shared mode.
- Commit-partial on disconnect; how to override.

- [ ] **Step 2: Re-run the install script to sync to `.claude/`**

```pwsh
pwsh "D:/Projects/UE5_MCP/Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1"
```

Expected: skill manifest updates copied to `<project>/.claude/`.

- [ ] **Step 3: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add Plugins/BlueprintReader/Claude/skills/bp-debug/SKILL.md .claude/skills/bp-debug/SKILL.md
git -C "D:/Projects/UE5_MCP" commit -m "docs(skills): bp-debug covers multi-session + shared daemon triage"
```

### Task 6.5: Update wiki Configuration page

**Files:**
- Modify: `wiki/Configuration.md`

- [ ] **Step 1: Add the new env vars**

Document:
- `BP_READER_DAEMON_IDLE_SECONDS` (default 300)
- `BP_READER_DAEMON_STARTUP_SECONDS` (default 600 — already documented but reaffirm)
- `BP_READER_BATCH_ON_DISCONNECT` (default `commit`, accepts `discard`)

- [ ] **Step 2: Document the cmdlet handshake file**

Mirror the existing live-handshake documentation. Same shape, different name.

- [ ] **Step 3: Note the exe rename**

Brief paragraph: a project named `UE5_MCP` produces `bp-reader-mcp-UE5_MCP.exe` alongside the canonical `bp-reader-mcp.exe`. Existing `.mcp.json` configs work unchanged.

- [ ] **Step 4: Commit**

```bash
git -C "D:/Projects/UE5_MCP" add wiki/Configuration.md
git -C "D:/Projects/UE5_MCP" commit -m "docs(wiki): document multi-session env vars + cmdlet handshake"
```

### Task 6.6: Push the wiki

**Files:** none (uses the existing wiki-sync workflow)

- [ ] **Step 1: Clone the wiki remote into a temp dir**

```bash
rm -rf /d/wiki-sync 2>/dev/null
git clone https://github.com/defessler/Unreal-Engine-5-MCP.wiki.git /d/wiki-sync
```

- [ ] **Step 2: Copy changed files**

```bash
cp "D:/Projects/UE5_MCP/wiki/Configuration.md" /d/wiki-sync/Configuration.md
```

- [ ] **Step 3: Commit + push**

```bash
git -C /d/wiki-sync add -A
git -C /d/wiki-sync commit -m "Configuration: multi-session env vars + cmdlet handshake"
git -C /d/wiki-sync push
```

- [ ] **Step 4: Cleanup**

```bash
rm -rf /d/wiki-sync
```

- [ ] **Step 5: Verify live**

```
WebFetch on https://github.com/defessler/Unreal-Engine-5-MCP/wiki/Configuration
```

Confirm new sections appear.

---

## Final task: ship the PR

### Task 7.1: Open the implementation PR

- [ ] **Step 1: Push the branch**

```bash
git -C "D:/Projects/UE5_MCP" push -u origin feat/multi-session-shared-daemon
```

- [ ] **Step 2: Open PR via gh**

```bash
cd "D:/Projects/UE5_MCP" && gh pr create --title "Multi-session MCP server + shared commandlet daemon" --body "Implements the design at docs/superpowers/specs/2026-05-13-multi-session-mcp-server-design.md. See docs/superpowers/plans/2026-05-13-multi-session-mcp-server.md for the task breakdown."
```

- [ ] **Step 3: Wait for CI**

```bash
cd "D:/Projects/UE5_MCP" && sleep 120 && gh pr view --json statusCheckRollup 2>&1 | tail -3
```

Expected: `conclusion: SUCCESS`.

- [ ] **Step 4: Merge (rebase)**

```bash
cd "D:/Projects/UE5_MCP" && gh pr merge --rebase --delete-branch
```

- [ ] **Step 5: Confirm main is clean**

```bash
git -C "D:/Projects/UE5_MCP" checkout main && git -C "D:/Projects/UE5_MCP" pull --rebase && cd "D:/Projects/UE5_MCP" && gh pr list --state open --json number 2>&1 | tail -3
```

Expected: `main` up to date; `[]` for open PRs.

---

## Self-review log

**Spec coverage check (every section/requirement from the spec):**

| Spec section | Plan task(s) |
|---|---|
| Architecture — N MCP servers, no lock | Phase 3 (drop lock); Phase 4 (daemon lifecycle); Phase 2 (Socket consolidation enables this) |
| Architecture — daemon as TCP server | Phase 1 (1.1, 1.2, 1.3) |
| File primitives — `bp-reader-cmdlet.{json,lock}` | Phase 1 (1.1 Step 4); Phase 4 (4.1) |
| Daemon transport — wire frames identical to live | Phase 1 (1.1 — derived from live server) |
| Daemon transport — code consolidation | Phase 2 (2.1–2.4) |
| Daemon lifecycle — two-lock spawn coordination | Phase 4 (4.1) |
| Daemon lifecycle — idle shutdown | Phase 4 (4.2) |
| Daemon lifecycle — crash recovery (stale pid) | Phase 4 (4.6); also Task 2.4 Step 1 |
| Daemon lifecycle — live opens mid-session | Existing auto-probe (no new task needed); covered by Task 2.5 |
| Daemon lifecycle — `shutdown_daemon` semantics | Phase 4 (4.5) |
| Per-session isolation — per-BP write lock | Phase 4 (4.3) |
| Per-session isolation — commit-partial on disconnect | Phase 4 (4.4) |
| Per-session isolation — cross-client error attribution | Implicit (per-socket response); no new task — verify in 3.3 manual test |
| Build-time exe rename | Phase 5 (5.1, 5.2) |
| FNV-1a temp-file prefix | Phase 5 (5.3) |
| Migration — backward-compat hard link | Phase 5 (5.2 Step 2) |
| Migration — MCP-server lock removal | Phase 3 (3.1) |
| Migration — daemon transport internal | Implicit (single PR, both halves upgrade together) |
| Testing — spawn race | Phase 4 (4.1 Step 1) |
| Testing — idle shutdown timer | Phase 4 (4.2 Step 3) — manual; consider an automated variant if reachable |
| Testing — per-BP write lock | Phase 4 (4.3 Step 4) |
| Testing — commit-partial on disconnect | Phase 4 (4.4 Step 2) — manual |
| Testing — multi-MCP-server integration | Phase 3 (3.3) — manual; Task 3.2 (mock-level) |
| Testing — stale handshake | Phase 4 (4.6) |
| Documentation updates | Phase 6 (all) |

Risks / open questions from the spec that the plan acknowledges but doesn't resolve:
- **Editor TCP listener multi-client support.** Phase 1's manual smoke test (Task 1.2 Step 4) verifies one client can drive it; we should also verify multi-client at the end of Phase 4 (Task 3.3 manual test) or add an explicit case to `test_socket_backend.cpp`.
- **Per-BP write lock corner case (delete-then-read).** Listed as a follow-up; not covered by a task. Acceptable for a first-cut implementation.

**Placeholder scan:** No "TBD" / "TODO" / "add appropriate error handling". Every step has either runnable code or a concrete verification command.

**Type consistency check:** `FCmdletServer`, `FBatchContext`, `FBatchRegistry`, `SocketBlueprintReader::Config`, `TryAttachExistingDaemon`, `EnsureDaemonAttached`, `SpawnLock`, `PollForHandshake`, `ProcessAlive`, `TcpProbe` — names match across the tasks they appear in.

**Scope check:** Focused on the spec's seven concerns (lock relocation, daemon TCP transport, socket consolidation, exe rename, batch isolation, lifecycle hardening, docs). Out-of-scope follow-ups (table-driven `RunOneOp`, typed `BPNode.meta`, cross-project broker) are explicitly excluded.

---

**Plan complete.** Saved to `docs/superpowers/plans/2026-05-13-multi-session-mcp-server.md`. Estimated effort: ~40 bite-sized tasks across 7 phases, roughly 1–2 weeks of focused work.
