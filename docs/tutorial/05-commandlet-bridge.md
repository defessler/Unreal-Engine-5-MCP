# Chapter 5 — MCP server to child UE process

You now have two halves that can't talk to each other. The MCP server
answers `list_blueprints` from fixture JSON. The UE plugin's
commandlet, run by hand, returns the same shape from a real asset
registry. This chapter wires them together: the MCP server launches
`UnrealEditor-Cmd.exe` as a child process, captures its output, and
returns real Blueprint metadata to the agent.

By the end you'll be able to run, with the editor closed:

```pwsh
$env:BP_READER_BACKEND  = "commandlet"
$env:BP_READER_PROJECT  = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$env:BP_READER_ENGINE_DIR = "D:\Projects\Unreal Engine 5"
BlueprintReaderMcp.exe   # JSON-RPC over stdio
```

...and have `list_blueprints` return your actual `/Game/AI` assets.

## What we're building

A new backend, `CommandletBlueprintReader`, that implements the same
`IBlueprintReader` interface as the mock backend but, on each call:

1. Builds a Windows command line for `UnrealEditor-Cmd.exe`.
2. Spawns the process with `CreateProcessW`, inheriting two anonymous
   pipes for stdout/stderr.
3. Waits for the child to exit (with a timeout), draining the pipes.
4. Reads the JSON result from a temp file the commandlet was told to
   write.
5. Parses and returns the JSON.

One process per tool call. That's slow (UE editor startup is several
seconds), but it works without any extra plumbing on the UE side and
forms the foundation we'll later optimize into daemon mode (Chapter 9)
and the live backend (Chapter 10).

## Skeleton

Add a new translation unit under `Plugins/BlueprintReader/Tests/src/backends/`. The
header declares a `CommandletBlueprintReader` subclass of
`IBlueprintReader` plus a `Config` struct holding the engine dir,
the uproject path, a timeout, and an `editorConfig` string. One
private method (`RunOp(args) -> nlohmann::json`) does all the work;
each public method is a thin wrapper that builds an arg list and
calls it.

Construction validates the engine path and resolves the exact exe to
launch. UE's binary naming follows `UEBuildBinary.cs`: the base name
gets `-Cmd` appended before the extension. For a Development build
that's plain `UnrealEditor-Cmd.exe`; for DebugGame it becomes
`UnrealEditor-Win64-DebugGame-Cmd.exe`. Mismatched config means UE
silently skips loading your plugin DLL, so we resolve it up front
rather than after a confusing failure:

```cpp
const auto binDir = cfg_.engineDir / "Engine" / "Binaries" / "Win64";
const std::string& cfgName = cfg_.editorConfig;
std::filesystem::path candidate =
    (cfgName.empty() || cfgName == "Development")
        ? binDir / "UnrealEditor-Cmd.exe"
        : binDir / fmt::format("UnrealEditor-Win64-{}-Cmd.exe", cfgName);
if (!std::filesystem::exists(candidate)) {
    throw BlueprintReaderError(fmt::format(
        "UnrealEditor-Cmd ({} config) not found at: {}",
        cfgName.empty() ? "Development" : cfgName, candidate.string()));
}
editorCmdExe_ = candidate;
```

## Argv on Windows

`CreateProcessW` takes a single `lpCmdLine` string, and the child's
`CommandLineToArgvW` parses it back into argv. Microsoft's parsing
rules are quirky: backslashes preceding a quote double, an embedded
quote becomes `\"`, and arguments without whitespace pass through
bare. We need round-trip-safe quoting:

```cpp
std::wstring QuoteWindowsArg(std::wstring_view in) {
    if (!in.empty() && in.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(in);
    }
    std::wstring out;
    out.push_back(L'"');
    for (std::size_t i = 0; i < in.size();) {
        std::size_t backslashes = 0;
        while (i < in.size() && in[i] == L'\\') { ++backslashes; ++i; }
        if (i == in.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (in[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            ++i;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(in[i]);
            ++i;
        }
    }
    out.push_back(L'"');
    return out;
}
```

That handles positional args — the uproject path comes through as one
argv entry even if the user installed UE under
`C:\Unreal Projects\Engine\`. We'll come back to flag-style args
(`-Key=Value`) below; UE's `FParse::Value` parses those itself, and
its rules are different from Windows'.

## Pipes and CreateProcessW

```cpp
SECURITY_ATTRIBUTES sa{};
sa.nLength = sizeof(sa);
sa.bInheritHandle = TRUE;

HANDLE outR = nullptr, outW = nullptr;
HANDLE errR = nullptr, errW = nullptr;
if (!CreatePipe(&outR, &outW, &sa, 0)) {
    throw BlueprintReaderError("CreatePipe(stdout) failed");
}
if (!CreatePipe(&errR, &errW, &sa, 0)) {
    CloseHandle(outR); CloseHandle(outW);
    throw BlueprintReaderError("CreatePipe(stderr) failed");
}
SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
```

`bInheritHandle = TRUE` on the SECURITY_ATTRIBUTES is what makes the
child inherit the write ends. We immediately clear the inherit flag
on our (parent) read ends with `SetHandleInformation`, so if the
child itself spawns subprocesses they don't accidentally inherit
handles into the parent's pipes.

Hand the write ends to the child as its stdout/stderr:

```cpp
STARTUPINFOW si{};
si.cb = sizeof(si);
si.dwFlags = STARTF_USESTDHANDLES;
si.hStdOutput = outW;
si.hStdError = errW;
si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

PROCESS_INFORMATION pi{};
BOOL ok = CreateProcessW(
    editorCmdExe_.wstring().c_str(),
    cmd.data(),     // built below
    nullptr, nullptr,
    TRUE,                       // inherit handles
    CREATE_NO_WINDOW,
    nullptr, nullptr,
    &si, &pi);
CloseHandle(outW);
CloseHandle(errW);
```

Close the parent's copies of the write ends right after the child
launches. Otherwise the pipe never reaches EOF when the child exits
(the parent still holds a write handle), and our read loop hangs
forever.

## Wait + drain

A child that writes a lot to a pipe will block once the kernel buffer
fills, so we drain on every wait tick:

```cpp
auto drain = [](HANDLE h, std::string& tail) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return;
        if (avail == 0) return;
        char buf[1024];
        DWORD got = 0;
        DWORD toRead = (avail > sizeof(buf)) ? (DWORD)sizeof(buf) : avail;
        if (!ReadFile(h, buf, toRead, &got, nullptr) || got == 0) return;
        AppendTail(tail, buf, got);
    }
};

auto deadline = std::chrono::steady_clock::now() + cfg_.timeout;
for (;;) {
    DWORD wr = WaitForSingleObject(pi.hProcess, 100);
    drain(outR, res.stdoutTail);
    drain(errR, res.stderrTail);
    if (wr == WAIT_OBJECT_0) break;
    if (std::chrono::steady_clock::now() >= deadline) {
        TerminateProcess(pi.hProcess, 9);
        WaitForSingleObject(pi.hProcess, 2000);
        res.timedOut = true;
        break;
    }
}
drain(outR, res.stdoutTail);   // final drain after the wait loop exits
drain(errR, res.stderrTail);

DWORD code = 0xFFFFFFFF;
GetExitCodeProcess(pi.hProcess, &code);
res.exitCode = code;
CloseHandle(pi.hProcess);
CloseHandle(pi.hThread);
```

Two details worth flagging. First, the final drain after the loop
exits — anything the child writes between our last in-loop drain and
its exit (the typical case: "shutdown complete" right before exit)
would otherwise be lost from the stderr tail, which is exactly what
we'd want in an error message.

Second, `AppendTail` caps the captured stderr at 8 KB. UE editors
are chatty; even on a successful call you'll get thousands of log
lines. The helper keeps only the tail and cuts at newline boundaries
so we never leave a half-codepoint at the start.

## Why a temp file, not stdout?

You've now got the child's stdout in `res.stdoutTail` and you might
expect to read the JSON result from there. Don't. UE installs its own
log device early in startup, and `FPlatformMisc::LocalPrint` plus
several other "obvious" stdout paths actually go through that
redirected device, not the real OS pipe. Even direct `fputs(stdout)`
hits the wrong sink in some configurations.

You can fight that battle (and Chapter 9's daemon mode does, by
calling `WriteFile` on the raw stdout handle from `GetStdHandle`), but
for per-call commandlet invocations a much simpler approach wins:
tell the commandlet to write its result to a temp file, then read the
file.

```cpp
std::filesystem::path TempJsonPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::filesystem::path tmp = (n == 0)
        ? std::filesystem::path(L"C:\\Windows\\Temp")
        : std::filesystem::path(std::wstring(buf, n));
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream name;
    name << "bp-reader-" << std::hex << dist(rng) << ".json";
    return tmp / name.str();
}
```

The plugin commandlet you wrote in Chapter 3 already honours
`-Out=<path>` — it writes the wire JSON to that file. Now we just
have to pass it.

See [../design/05-backends.md](../design/05-backends.md) for the full
discussion of why the temp-file approach won out over piping JSON
through a delimiter on stdout.

## Putting it together: RunOp

```cpp
nlohmann::json CommandletBlueprintReader::RunOp(
    const std::vector<std::wstring>& opArgs)
{
    auto outFile = TempJsonPath();

    std::vector<std::wstring> args;
    args.push_back(cfg_.uproject.wstring());      // positional: .uproject
    args.push_back(L"-run=BPR");
    for (const auto& a : opArgs) args.push_back(a);
    args.push_back(L"-Out=" + outFile.wstring());
    args.push_back(L"-Compact");
    args.push_back(L"-nullrhi");
    args.push_back(L"-nosplash");
    args.push_back(L"-unattended");
    args.push_back(L"-nopause");
    args.push_back(L"-stdout");

    auto r = RunChild(editorCmdExe_.wstring(), args, cfg_.timeout);

    if (!r.launched) {
        throw BlueprintReaderError(fmt::format(
            "failed to launch UnrealEditor-Cmd.exe: {}", r.failureReason));
    }
    if (r.timedOut) {
        throw BlueprintReaderError(fmt::format(
            "commandlet timed out after {}s; tail of stderr:\n{}",
            cfg_.timeout.count(), TrimLines(r.stderrTail, 250)));
    }
    if (r.exitCode != 0) {
        std::string tail = TrimLines(
            r.stderrTail.empty() ? r.stdoutTail : r.stderrTail, 250);
        if (r.exitCode == 4) {
            throw AssetNotFound(fmt::format(
                "commandlet reported missing target (exit=4); tail:\n{}", tail));
        }
        throw BlueprintReaderError(fmt::format(
            "commandlet exit={}; tail:\n{}", r.exitCode, tail));
    }

    if (!std::filesystem::exists(outFile)) {
        throw BlueprintReaderError(fmt::format(
            "commandlet exited 0 but produced no output file at {}",
            outFile.string()));
    }

    nlohmann::json parsed;
    std::ifstream in(outFile);
    in >> parsed;
    std::filesystem::remove(outFile);
    return parsed;
}
```

A few things to notice:

- The uproject path is a positional arg; everything after `-run=`
  is a UE-style flag.
- `-nullrhi -nosplash -unattended -nopause` are standard UE flags
  that skip the renderer init, the splash screen, anything modal,
  and the "press any key" prompt at exit. Each of those would hang
  an unattended commandlet run.
- Exit code 4 is the convention the plugin uses for "asset not found".
  We surface that as a distinct exception type so the dispatcher
  layer can return a clean MCP error instead of a stack trace.

## Two read tools, end to end

With `RunOp` in place, `ListBlueprints` and `ReadBlueprint` are tiny:

```cpp
std::vector<BPAssetSummary>
CommandletBlueprintReader::ListBlueprints(std::string_view path) {
    std::vector<std::wstring> args;
    args.push_back(L"-Op=List");
    if (!path.empty()) {
        args.push_back(L"-Path=" + Widen(path));
    }
    auto j = RunOp(args);
    return j.get<std::vector<BPAssetSummary>>();
}

BPMetadata
CommandletBlueprintReader::ReadBlueprint(std::string_view assetPath) {
    auto j = RunOp({
        L"-Op=Read",
        L"-Asset=" + Widen(assetPath),
    });
    return j.get<BPMetadata>();
}
```

The `j.get<T>()` calls deserialize using the `from_json` overloads
from Chapter 4. Same wire shape as the mock backend.

## The FParse trap: encoding flag args

`-Op=List` works. `-Asset=/Game/AI/BP_TestEnemy` works. What about
`-Query=Dummy Targets`? The naive form is `-Query=Dummy Targets`, but
`CommandLineToArgvW` splits at the space; the child sees argv entries
`-Query=Dummy` and `Targets`, and `FParse::Value` reads only "Dummy".

You might try the obvious fix: wrap with Windows-style outer quotes,
`"-Query=Dummy Targets"`. The OS now keeps the arg as one entry, but
UE's `FParse::Value` doesn't understand Windows escaping — it looks
for a quote *immediately after* the `=` sign. With our outer-quote
form the value seen by FParse is `Dummy Targets`, which it splits on
the space anyway.

The fix is to inner-quote: emit `-Query="Dummy Targets"`. FParse's
quoted-value path triggers when the char after `=` is `"`, reads
until the next `"`, and gives us the whole value. We need this only
for flag args; positional args use the Windows form.

```cpp
inline std::wstring EncodeArgForFParse(std::wstring_view arg) {
    auto eq = arg.find(L'=');
    if (eq == std::wstring_view::npos) return std::wstring(arg);
    std::wstring_view key   = arg.substr(0, eq);    // includes leading "-"
    std::wstring_view value = arg.substr(eq + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        return std::wstring(arg);                   // already quoted
    }
    if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);                   // bare value is fine
    }
    std::wstring out;
    out.append(key);
    out.push_back(L'=');
    out.push_back(L'"');
    for (wchar_t c : value) {
        if (c == L'"') continue;  // FParse has no escape; strip defensively
        out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}
```

Then a per-arg dispatcher picks the right strategy:

```cpp
inline std::wstring EncodeArg(std::wstring_view arg) {
    const bool bLooksLikeOption =
        !arg.empty() && arg.front() == L'-' &&
        arg.find(L'=') != std::wstring_view::npos;
    if (bLooksLikeOption) return EncodeArgForFParse(arg);
    return QuoteWindowsArg(arg);
}
```

The `-` prefix check is load-bearing. A version that just looks for
`=` would mis-route a positional uproject path containing an `=` in
its directory name (`C:\Foo=Bar\Game.uproject`, a legal NTFS path)
into the FParse encoder and break launch for that user.

Wire the encoder into `BuildCommandLine` and you're done with arg
handling:

```cpp
std::wstring BuildCommandLine(const std::wstring& exe,
                              const std::vector<std::wstring>& args) {
    std::wstring cmd = QuoteWindowsArg(exe);
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd.append(EncodeArg(a));
    }
    return cmd;
}
```

Keep the encoder in a header so doctest can exercise it directly,
without spinning up an editor.

## Wiring into the backend selector

In Chapter 1 you stubbed a backend factory that always returned the
mock backend. Now teach it to honour `BP_READER_BACKEND`:

```cpp
std::unique_ptr<IBlueprintReader> MakeBackend() {
    const char* mode = std::getenv("BP_READER_BACKEND");
    std::string m = mode ? mode : "mock";
    if (m == "commandlet") {
        CommandletBlueprintReader::Config cfg;
        cfg.engineDir = std::getenv("BP_READER_ENGINE_DIR");
        cfg.uproject  = std::getenv("BP_READER_PROJECT");
        cfg.useDaemon = false;   // single-shot for now
        return std::make_unique<CommandletBlueprintReader>(std::move(cfg));
    }
    return std::make_unique<MockBlueprintReader>(FixturesDir());
}
```

`auto` and `live` come in Chapter 11. For now, `mock` and
`commandlet` are enough.

## Checkpoint

With the editor closed, run:

```pwsh
$env:BP_READER_BACKEND   = "commandlet"
$env:BP_READER_PROJECT   = "D:\Projects\UE5_MCP\UE5_MCP.uproject"
$env:BP_READER_ENGINE_DIR = "D:\Projects\Unreal Engine 5"

<removed in the UBT migration; use BlueprintReaderMcpTests.exe instead> `
    -Tool list_blueprints -Args '{"path":"/Game/AI"}'
```

The script feeds a `tools/call` JSON-RPC frame to the server over
stdio and prints the response. You should see something like:

```json
{
  "result": {
    "content": [{
      "type": "text",
      "text": "[{\"asset_path\":\"/Game/AI/BP_TestEnemy\",\"parent_class\":\"/Script/Engine.Character\"}, ...]"
    }],
    "_meta": {"elapsed_ms": 8421, "tool": "list_blueprints"}
  }
}
```

Eight seconds is correct for one-shot mode — that's UE editor
startup. We fix that in Chapter 9 with daemon mode. For now: real
assets, real backend, real bridge. Move on to the first write.
