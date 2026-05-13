# Chapter 9 — Long-lived editor daemon

By the end of Chapter 8 you have a single-process MCP server, a UE
plugin commandlet, write ops, and `apply_ops` batching. Every call
follows the same shape: MCP spawns `UnrealEditor-Cmd.exe`, the
commandlet runs one op, writes its JSON result to a temp file, then
exits. The shell-out is the bottleneck.

## The problem

A cold-start of `UnrealEditor-Cmd.exe` on a modestly-sized project is
not cheap. On the reference machine, time-to-first-byte for a single
`-Op=List` call breaks down roughly like this:

| Phase | Time |
|---|---|
| `CreateProcessW` + DLL loads | ~700 ms |
| UE module bring-up | 1.5 – 3 s |
| Asset registry scan | 1 – 3 s |
| Plugin loading | 500 ms – 1 s |
| Actual op | 5 – 50 ms |
| Process teardown | 200 – 400 ms |
| **Total** | **5 – 10 s** |

5 – 10 seconds per call is hostile to interactive use. An agent that
issues a dozen tool calls to read a graph, add a variable, wire two
pins, and inspect the result is staring at 60 – 120 seconds of wall
time before any of its work happens. We need the editor to stay
resident across calls.

## The shape of the fix

Keep one editor process alive. Talk to it across calls. The
commandlet already knows how to dispatch a single op; teach it to
read multiple ops from its own stdin and emit a sentinel after each
one so the MCP-server side knows when to read the result.

This is the **daemon mode**:

- Plugin: `-Daemon` flag on the commandlet command line switches from
  `RunOneOp` (one shot, then exit) to `RunDaemon` (loop on stdin).
- MCP server: spawn the daemon once, hold its stdin/stdout pipes,
  write a newline-delimited arg string per call, scan stdout for a
  sentinel.

Per-call cost drops to "the op itself" plus a few milliseconds of
pipe overhead.

## Plugin side: `RunDaemon`

The commandlet's `Main` already calls `RunOneOp(Params)` for one-shot
use. Wrap that in a `Daemon` check before the dispatch:

```cpp
// BlueprintReaderCommandlet.cpp
int32 UBlueprintReaderCommandlet::Main(const FString& Params)
{
    if (FParse::Param(*Params, TEXT("Daemon")))
    {
        return RunDaemon();
    }
    return RunOneOp(Params);
}
```

`RunDaemon` is the new loop. It reads one line per call, dispatches
through `RunOneOp` (the same one-shot path — same code, same temp
file `-Out=` flow), and writes a `__BPR_DONE <code>__\n` sentinel
after each op so the MCP server knows the result file is ready.

Cold-start work happens once on entry: UE has already loaded its
modules, scanned the asset registry, and brought up our plugin. Every
subsequent op pays just the op-itself cost.

### The stdio gotcha (this is the load-bearing detail)

UE's runtime redirects C stdio through its own log/output system in
several configs. `fputs(stdout, ...)`, `printf`, even
`FPlatformMisc::LocalPrint` can land in the wrong place — not the
real stdout pipe the MCP-server side is reading from. If you take
that route, the sentinel never reaches the MCP-server scanner and
every call hangs at the read.

The fix is to bypass UE's redirection entirely by going straight to
the Win32 console API:

```cpp
// BlueprintReaderCommandlet.cpp — inside RunDaemon
//
// Use Windows API directly for stdio so UE's runtime redirection
// (which can route C stdio through its log/output system) doesn't
// intercept the stream. We need raw bytes hitting the pipe in both
// directions.
HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);

auto WriteAll = [hOut](const char* s, DWORD n)
{
    while (n > 0)
    {
        DWORD wrote = 0;
        if (!WriteFile(hOut, s, n, &wrote, nullptr) || wrote == 0) return false;
        s += wrote;
        n -= wrote;
    }
    return true;
};
```

`GetStdHandle(STD_OUTPUT_HANDLE)` returns the raw kernel handle to the
stdout pipe — the same pipe the MCP-server side created with
`CreatePipe`. Nothing in UE's log machinery sits between us and that
handle. `WriteFile` does the actual write.

The same pattern applies to stdin: read one byte at a time with
`ReadFile(hIn, ...)`, accumulate until newline, dispatch.

This gotcha is documented in [CLAUDE.md](../../CLAUDE.md) under "UE
stdio in commandlet mode." If you ever see the MCP-server side
timing out waiting for a sentinel while UE clearly logged something,
this is the first place to look.

### The ready sentinel

The MCP-server side needs to know when the daemon has finished its
cold-start and is ready to accept ops. `RunDaemon` emits a one-time
`__BPR_READY__\n` line as soon as it's looping:

```cpp
const char ready[] = "__BPR_READY__\n";
WriteAll(ready, (DWORD)(sizeof(ready) - 1));
```

The MCP side blocks on this marker before declaring the daemon usable
(see "Startup wait" below).

### The op loop

```cpp
auto ReadLine = [hIn](FString& Out) -> bool
{
    Out.Reset();
    char ch;
    while (true)
    {
        DWORD got = 0;
        if (!ReadFile(hIn, &ch, 1, &got, nullptr) || got == 0)
        {
            return !Out.IsEmpty();  // EOF: return what we have if any
        }
        if (ch == '\r') continue;
        if (ch == '\n') return true;
        Out.AppendChar(static_cast<TCHAR>(ch));
    }
};

while (true)
{
    FString Line;
    if (!ReadLine(Line)) return 0;  // stdin closed → clean exit

    Line.TrimStartAndEndInline();
    if (Line.IsEmpty()) continue;
    if (Line.Equals(TEXT("QUIT"), ESearchCase::IgnoreCase)) return 0;

    const int32 Code = RunOneOp(Line);
    const FString DoneStr = FString::Printf(TEXT("__BPR_DONE %d__\n"), Code);
    const auto DoneAnsi = StringCast<ANSICHAR>(*DoneStr);
    WriteAll(DoneAnsi.Get(), (DWORD)FCStringAnsi::Strlen(DoneAnsi.Get()));
}
```

Notice the inner-line conventions:

- One newline per op. The MCP-server side scans for `\n` to know a
  line is ready.
- `\r` is dropped — defensive against any layer that might inject
  Windows-style line endings.
- `QUIT` (case-insensitive) is a clean shutdown signal. The MCP side
  uses this in `TerminateDaemon` to ask politely before sending
  `TerminateProcess`.
- The sentinel is `__BPR_DONE <code>__\n`. The `<code>` is the int
  return from `RunOneOp` — 0 on success, non-zero on per-op failure.
  Crucially: **don't log this string anywhere else**. The MCP-side
  scanner finds the first occurrence and parses the integer after
  the space. A help text mentioning `__BPR_DONE <code>__` literally
  would be matched as `<code>__)` and break the next call. This is
  a real gotcha — the documentation that warned about it (in
  CLAUDE.md) is itself written with placeholders broken by spaces.

## MCP server side: long-lived child

The mock backend and the one-shot commandlet backend already have the
shape: build args, spawn child, read result, return. The daemon
backend keeps the child between calls and re-uses the pipes.

### State on `CommandletBlueprintReader`

```cpp
// CommandletBlueprintReader.cpp (state on the class)
HANDLE daemonProcess_ = nullptr;
HANDLE daemonStdin_   = nullptr;
HANDLE daemonStdout_  = nullptr;
std::string accumulator_;  // read buffer; scan finds markers here
std::mutex  daemonMutex_;  // serialize cross-thread access
```

`accumulator_` is the key data structure. We never block waiting for
"exactly one frame" because daemon output isn't framed at the byte
level. We read whatever's available into `accumulator_`, then scan
for the sentinel.

### Spawning the daemon

`EnsureDaemon` builds the same arg list the one-shot path uses, plus
`-Daemon`:

```cpp
std::vector<std::wstring> args;
args.push_back(cfg_.uproject.wstring());
args.push_back(L"-run=BlueprintReader");
args.push_back(L"-Daemon");
args.push_back(L"-nullrhi");
args.push_back(L"-nosplash");
args.push_back(L"-unattended");
args.push_back(L"-nopause");
args.push_back(L"-stdout");
```

`-nullrhi` skips the renderer; we never need a viewport in a daemon.
`-unattended` makes UE non-interactive. `-stdout` re-enables stdout
output that some plugins suppress.

Spawn with `CreateProcessW`, redirected stdio:

```cpp
HANDLE childInR, childInW;
HANDLE childOutR, childOutW;
CreatePipe(&childInR, &childInW, &sa, 0);
CreatePipe(&childOutR, &childOutW, &sa, 0);
SetHandleInformation(childInW,  HANDLE_FLAG_INHERIT, 0);
SetHandleInformation(childOutR, HANDLE_FLAG_INHERIT, 0);

STARTUPINFOW si{};
si.cb = sizeof(si);
si.dwFlags = STARTF_USESTDHANDLES;
si.hStdInput  = childInR;
si.hStdOutput = childOutW;
si.hStdError  = childOutW;  // merge stderr into stdout
```

The stderr → stdout merge means we get one stream to scan. Plugin log
output and our sentinels go through the same pipe; the scanner
finds the sentinel and the rest is diagnostic noise we accumulate
for error messages.

### Startup wait

After launch, block until `__BPR_READY__\n` appears in
`accumulator_`. Large UE projects (lots of plugins, cold DDC) can
take 30+ seconds to cold-start, so this gets its own timeout
distinct from the per-op timeout:

```cpp
ReadUntilMarker("__BPR_READY__\n", cfg_.startupTimeout);
```

`ReadUntilMarker` is the workhorse. It loops on `PeekNamedPipe` +
`ReadFile`, appends bytes to `accumulator_`, and returns the prefix
up to (but not including) the marker as soon as it appears. It also
watches `WaitForSingleObject(daemonProcess_, 0)` for child death —
if the editor crashes during startup we detect that and surface a
diagnostic rather than blocking forever.

### Per-op call

```cpp
nlohmann::json CommandletBlueprintReader::RunOpDaemon(
    const std::vector<std::wstring>& opArgs)
{
    std::lock_guard<std::mutex> lock(daemonMutex_);
    EnsureDaemon();

    auto outFile = TempJsonPath();
    std::wstring line;
    for (const auto& a : opArgs)
    {
        if (!line.empty()) line.push_back(L' ');
        line.append(EncodeArgForFParse(a));
    }
    line += L" -Out=\"" + outFile.wstring() + L"\" -Compact\n";

    // Write line to the daemon's stdin.
    auto ansi = WideToUtf8(line);
    DWORD written = 0;
    WriteFile(daemonStdin_, ansi.data(), (DWORD)ansi.size(), &written, nullptr);

    // Wait for the per-op sentinel.
    auto consumed = ReadUntilMarker("__BPR_DONE ", cfg_.timeout);
    // Then read the exit code up to "__\n".
    auto codeStr = ReadUntilMarker("__\n", cfg_.timeout);
    int code = std::stoi(codeStr);

    // Load + delete the per-op temp file. The plugin's EmitJson wrote
    // to outFile; the contents are the canonical wire JSON.
    auto j = LoadJsonFile(outFile);
    std::filesystem::remove(outFile);
    if (code != 0) throw BlueprintReaderError("op failed");
    return j;
}
```

The per-call temp file is the same machinery `RunOneOp` uses in
one-shot mode. We don't try to stream the JSON inline over the pipe
— UE log lines can interleave with our output and corrupt a JSON
parser. Writing to disk is reliable and fast (sub-millisecond on a
modern SSD for these payloads).

### Cleanup

`TerminateDaemon` sends `QUIT\n` to give the daemon a chance to
exit cleanly, waits 2 seconds, then `TerminateProcess` if needed:

```cpp
const char* quit = "QUIT\n";
DWORD written = 0;
WriteFile(daemonStdin_, quit, 5, &written, nullptr);
CloseHandle(daemonStdin_);
daemonStdin_ = nullptr;

if (WaitForSingleObject(daemonProcess_, 2000) != WAIT_OBJECT_0)
{
    TerminateProcess(daemonProcess_, 0);
    WaitForSingleObject(daemonProcess_, 1000);
}
CloseHandle(daemonProcess_);
daemonProcess_ = nullptr;
```

Closing the daemon's stdin handle is what triggers the `ReadFile` in
`RunDaemon` to return `got == 0` (EOF), which exits the loop. This
is the "polite" shutdown path that matters when the daemon held a
write transaction in `apply_ops` open — `TerminateProcess` mid-write
would leak temp files and possibly corrupt a `.uasset` save.

### Opt-out

Some users want the old one-shot behavior (e.g. CI scripts that
expect deterministic per-call isolation). Honor `BP_READER_DAEMON=0`:

```cpp
// CommandletBlueprintReader::RunOp
if (cfg_.useDaemon) {
    try {
        return RunOpDaemon(opArgs);
    } catch (const BlueprintReaderError& e) {
        // Daemon transport failure — log and fall through to one-shot.
        std::fprintf(stderr,
            "[bp-reader-mcp][commandlet][daemon] transport error, "
            "falling back to one-shot: %s\n", e.what());
        TerminateDaemon();
    }
}
// One-shot path: spawn the editor for this single op and wait.
return RunOpOneShot(opArgs);
```

Default is on. A transport failure (broken pipe, child crash) is
also a chance to fall back gracefully — the user's call shouldn't
fail because the daemon's transport is degraded; respawning is just
a slower form of "spawn one editor process," which we know works.

## Single-instance lock

Two MCP-server instances spawning two daemons against the same
project is a recipe for `.uasset` file lock contention. We guard
against it with an OS-level exclusive file lock keyed by a hash of
the absolute project path. The detailed implementation lives in
Chapter 13; here we just note that the lock exists and that the
daemon's lifetime is one-to-one with the MCP-server's lifetime.

## Telemetry

Every `tools/call` envelope already carries
`_meta: {elapsed_ms, tool}` from Chapter 4. The daemon doesn't
change the shape — `elapsed_ms` measures the same wall-clock window
(MCP-server-side, from "request received" to "result emitted"). What
changes is the distribution.

## Checkpoint

Start the MCP server. The first call is the daemon cold-start, so
expect 5 – 10 seconds. Subsequent calls in the same session should
drop to tens of milliseconds. Verify by reading `_meta.elapsed_ms`
from the JSON-RPC response:

```jsonc
// First call after MCP startup:
{ "result": {...}, "_meta": { "tool": "list_blueprints", "elapsed_ms": 7843 } }

// Second call, same session:
{ "result": {...}, "_meta": { "tool": "read_blueprint",  "elapsed_ms": 38 } }

// Tenth call:
{ "result": {...}, "_meta": { "tool": "add_variable",    "elapsed_ms": 24 } }
```

Confirm the daemon is sharing process: `Get-Process UnrealEditor-Cmd`
between calls shows exactly one process, and its PID stays the same
across the session. Kill it and the next call should respawn it (and
take cold-start time again).

Set `BP_READER_DAEMON=0`, restart the MCP server, and re-run the
sequence. Every call should now show 5 – 10 second elapsed time —
back to the pre-daemon baseline. Reset the env var and confirm the
ms numbers come back.

You now have an editor process that lives across calls and a wire
protocol robust to UE's stdio quirks. Chapter 10 introduces the
**live** backend: instead of spawning an editor we don't see, talk to
the editor the developer already has open.

See also: [design/05-backends.md](../design/05-backends.md) for the
backend taxonomy and the transport-failure recovery rules.
