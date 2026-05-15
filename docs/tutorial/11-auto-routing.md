# Chapter 11 — Auto backend

By the end of Chapter 10 you have two write-capable backends:

- `live` — talks to a running editor over TCP. Fast, sees in-memory
  state, but only works while the editor is open.
- `commandlet` — spawns its own `UnrealEditor-Cmd.exe` (daemonized).
  Works without an open editor, but fights with one for `.uasset`
  locks.

The user shouldn't have to pick. Most workflows look like:

> The editor's open while I'm iterating. I close it to free memory
> when I'm cooking, running an LLM batch task, or just stepping
> away. The MCP server is up the entire time — sometimes the editor
> is up alongside, sometimes not.

The auto backend handles that transparently. Each call probes the
state of the world ("is there an editor listening?") and routes to
the appropriate sub-backend. No client config change when the editor
opens or closes.

## Strategy

Per-call dispatch with a short TTL on the probe result:

```
For each tool call:
  1. If <2 s since last probe, reuse the last decision.
  2. Otherwise:
     a. Try to read <Project>/Saved/bp-reader-live.json.
     b. If present, do a non-blocking TCP connect probe to the port.
     c. If the connect succeeds, route to Live.
     d. Otherwise, route to Commandlet.
  3. Forward the IBlueprintReader call to the chosen backend.
```

The 2-second cache (`probeTtl` in the config) is what keeps a burst
of 50 calls from doing 50 TCP probes. Bursts of fast calls
(`apply_ops` payloads, agent loops) reuse one decision. The cache
is short enough that the user closing the editor is detected within
a few seconds — fast enough that it feels instantaneous in
interactive use.

Cache invalidation: any backend transport failure clears the cache
and forces a fresh probe on the next call. We never get stuck routing
to a dead live editor for the full TTL.

## `AutoBlueprintReader`

The class lives in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/backends/AutoBlueprintReader.{h,cpp}`.
It implements `IBlueprintReader` by holding optional pointers to
both sub-backends and a probe-state struct, then forwarding every
method to whichever one `Pick()` returns.

### Pick

```cpp
// AutoBlueprintReader.cpp
IBlueprintReader& AutoBlueprintReader::Pick() {
    auto now = std::chrono::steady_clock::now();
    if (now - lastProbe_ >= cfg_.probeTtl) {
        Probe();
    }
    if (useLive_ && live_) return *live_;
    return EnsureCommandlet();
}
```

`Pick()` is the single decision point. It checks the TTL, re-probes
if needed, and returns a reference to whichever sub-backend is
current.

`EnsureCommandlet()` is lazy — we don't pay for `CommandletBlueprintReader`
construction (which validates the engine dir and editor exe on
disk) until we actually need it. A session that finds the editor
already open on every probe will never construct the commandlet at
all.

### Probe

```cpp
void AutoBlueprintReader::Probe() {
    // Build a candidate live config. Empty → no editor known about.
    auto candidate = TryBuildLive();
    if (!candidate) {
        useLive_ = false;
        live_.reset();
        lastProbe_ = std::chrono::steady_clock::now();
        return;
    }

    // The candidate has a port + token; confirm something is actually
    // listening on the port before we commit. A stale handshake file
    // from a crashed editor would otherwise route us to a broken Live
    // until the file ages out.
    std::string probeHost;
    int probePort = 0;
    auto hs = ReadHandshake(cfg_.uproject);
    if (cfg_.livePort != 0) {
        probePort = cfg_.livePort;
        probeHost = cfg_.liveHost.empty() ? "127.0.0.1" : cfg_.liveHost;
    } else if (hs) {
        probePort = hs->port;
        probeHost = hs->host.empty() ? "127.0.0.1" : hs->host;
    }
    bool reachable = probePort > 0 &&
                     TcpProbe(probeHost, probePort, cfg_.probeConnectTimeout);
    if (!reachable) {
        useLive_ = false;
        live_.reset();
        lastProbe_ = std::chrono::steady_clock::now();
        return;
    }

    if (!live_) live_ = std::move(candidate);
    useLive_ = true;
    lastProbe_ = std::chrono::steady_clock::now();
}
```

Two checks per probe: (1) is there a handshake file with a sensible
port and token, and (2) is anything actually listening on that
port. The second check matters: a crashed editor leaves a stale
handshake file behind for up to several seconds until our cleanup
catches up. Without the TCP probe, we'd happily build a
`LiveBlueprintReader` against the dead port and only discover the
problem when the first op times out.

### TCP probe

```cpp
bool TcpProbe(const std::string& host, int port,
              std::chrono::milliseconds timeout) {
    // Create a non-blocking socket, attempt connect, select() on
    // writability with timeout. SO_ERROR after select tells us
    // whether the connect succeeded or got refused.
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1; ::ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) { /* immediate connect */ closeSock(); return true; }

    fd_set wfds;
    FD_ZERO(&wfds); FD_SET(s, &wfds);
    timeval tv;
    tv.tv_sec  = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    rc = ::select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
    if (rc <= 0) { closeSock(); return false; }

    int err = 0;
    int errLen = sizeof(err);
    ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&err), &errLen);
    closeSock();
    return err == 0;
}
```

Non-blocking + `select` is how you do a connect with a budget.
`probeConnectTimeout` defaults to 500 ms — long enough to tolerate
a busy listener, short enough that a dead probe doesn't stall an
interactive call.

We deliberately don't run the **auth** handshake here. That's
`LiveBlueprintReader`'s job on the first real call. The probe just
answers "is there a TCP listener on that port" — a simpler question,
robust to handshake-protocol changes.

### Building live with handshake-file path

```cpp
std::unique_ptr<LiveBlueprintReader> AutoBlueprintReader::TryBuildLive() {
    LiveBlueprintReader::Config lc;
    lc.host  = cfg_.liveHost.empty() ? "127.0.0.1" : cfg_.liveHost;
    lc.port  = cfg_.livePort;
    lc.token = cfg_.liveToken;

    // Fill missing fields from the handshake file. The MCP server
    // re-reads the file (rather than caching from startup) so that an
    // editor restart with a fresh ephemeral port + token gets picked
    // up without restarting the MCP server.
    if (lc.port == 0 || lc.token.empty()) {
        if (auto hs = ReadHandshake(cfg_.uproject)) {
            if (lc.port == 0)      lc.port  = hs->port;
            if (lc.token.empty())  lc.token = hs->token;
            if (lc.host == "127.0.0.1" && !hs->host.empty()) lc.host = hs->host;
        }
    }
    if (lc.port == 0 || lc.token.empty()) return nullptr;

    // Same self-refresh wiring as the static `live` backend. Auto's
    // per-call probe handles editor-restart recovery at the outer
    // layer, but inner-layer refresh keeps a currently-live
    // LiveBlueprintReader usable across an editor restart that
    // happens between the probe and the next op.
    if (!cfg_.uproject.empty()) {
        lc.handshakeFilePath =
            (cfg_.uproject.parent_path() / "Saved" / "bp-reader-live.json").string();
    }
    return std::make_unique<LiveBlueprintReader>(std::move(lc));
}
```

Note the two layers of self-refresh:

- **Outer (Auto):** the 2-second probe catches editor open/close
  events. The cost is one TCP connect per 2-second window when the
  editor's state is steady.
- **Inner (Live):** within a single `LiveBlueprintReader` instance,
  the connect/auth retry on the handshake file catches editor
  restart that happens **between** the probe and the actual op.
  Without this layer, a probe at t=0 followed by an op at t=1 with
  an editor restart at t=0.5 would hit a stale port.

Both layers point at the same handshake file path.

## Forwarders

Every `IBlueprintReader` method forwards through `Pick()`:

```cpp
#define FORWARD(method, ...)                          \
    do {                                              \
        std::lock_guard<std::mutex> lock(mu_);        \
        return Pick().method(__VA_ARGS__);            \
    } while (0)

#define FORWARD_VOID(method, ...)                     \
    do {                                              \
        std::lock_guard<std::mutex> lock(mu_);        \
        Pick().method(__VA_ARGS__);                   \
    } while (0)

std::vector<BPAssetSummary> AutoBlueprintReader::ListBlueprints(std::string_view p) {
    FORWARD(ListBlueprints, p);
}
BPMetadata AutoBlueprintReader::ReadBlueprint(std::string_view a) {
    FORWARD(ReadBlueprint, a);
}
BPGraph AutoBlueprintReader::GetGraph(std::string_view a, std::string_view g) {
    FORWARD(GetGraph, a, g);
}
// ... and 50+ more, one for every tool.
```

The macro keeps the forwarders mechanical. Adding a tool to
`IBlueprintReader` means adding one `FORWARD` line here — both
sub-backends already implement the interface, so the auto wrapper
gets the new tool "for free." This is the load-bearing reason the
plugin/`mock`/`commandlet`/`live` foursome all derive from the same
`IBlueprintReader` interface.

The mutex around `Pick()` doesn't serialize the calls themselves
(both sub-backends do their own internal serialization where
needed). It serializes **probe state** — two threads racing to call
`Probe()` would otherwise both build candidate readers and one
would leak.

### Special case: shutdown_daemon

```cpp
nlohmann::json AutoBlueprintReader::ShutdownDaemon() {
    // Always route to commandlet — Live has no daemon to shut down,
    // and the user calling shutdown_daemon explicitly wants to release
    // the editor-daemon process locks (not affect the live editor).
    std::lock_guard<std::mutex> lock(mu_);
    if (!commandlet_) {
        return nlohmann::json{
            {"ok", true},
            {"was_running", false},
            {"hint", "auto backend never spawned a commandlet (live editor "
                     "covered every call this session)"},
        };
    }
    return commandlet_->ShutdownDaemon();
}
```

`shutdown_daemon` is the one tool that's about a specific backend's
process lifecycle rather than about the BP it acts on. It always
targets the commandlet (the only backend with a daemon process); the
live editor isn't ours to kill.

## Default policy

`BackendFactory` decides the default:

```cpp
// BackendFactory.cpp — LoadConfig
if (cfg.backend.empty()) {
    cfg.backend = cfg.uproject.empty() ? "mock" : "auto";
}
```

Two cases:

- **No `.uproject` auto-discoverable** (someone running the bare
  exe with no project context): default to `mock`. The mock backend
  works against a fresh checkout and gives the MCP server something
  responsive to talk to.
- **`.uproject` found**: default to `auto`. The user has a project;
  they almost certainly want to operate on it; auto routes them
  correctly without their having to learn the backend taxonomy.

Explicit `BP_READER_BACKEND=commandlet|live|mock` still wins. Users
who want deterministic single-backend behavior (CI, debugging the
mock fixtures, isolating a single backend's behavior) get it.

## Building Auto in `BackendFactory`

```cpp
if (cfg.backend == "auto") {
    AutoBlueprintReader::Config ac;
    ac.uproject = cfg.uproject;
    ac.liveHost = cfg.liveHost.empty() ? "127.0.0.1" : cfg.liveHost;
    ac.livePort = cfg.liveProcPort;
    ac.liveToken = cfg.liveToken;
    CommandletBlueprintReader::Config cc;
    cc.engineDir       = cfg.engineDir;
    cc.uproject        = cfg.uproject;
    cc.timeout         = std::chrono::seconds(cfg.timeoutSeconds);
    cc.startupTimeout  = std::chrono::seconds(cfg.startupTimeoutSeconds);
    cc.useDaemon       = cfg.useDaemon;
    cc.editorConfig    = cfg.editorConfig;
    cc.editorExtraArgs = cfg.editorExtraArgs;
    ac.commandletConfig = std::move(cc);
    ac.prewarmCommandlet = cfg.prewarm && cfg.useDaemon;
    return std::make_unique<AutoBlueprintReader>(std::move(ac));
}
```

Both sub-backend configs flow through `AutoBlueprintReader::Config`.
Prewarming is opt-in: when set, the auto backend eagerly constructs
and pre-spawns the commandlet daemon at startup so the first
commandlet-routed call doesn't pay cold-start cost. Most users
don't enable it (the live editor is more common than not), but
it's there for CI / long-running batch workflows.

## Test seam

Auto routing decisions are state-dependent, which makes them
annoying to test. We expose a hook:

```cpp
std::string AutoBlueprintReader::SelectBackendForTesting() {
    std::lock_guard<std::mutex> lock(mu_);
    lastProbe_ = std::chrono::steady_clock::time_point{};  // force re-probe
    Probe();
    return useLive_ ? "live" : "commandlet";
}
```

Tests call this and assert "live" vs "commandlet" based on
environment setup (handshake file present? listener on the port?).
The TTL-bypass means the test doesn't need to wait for cache
expiry.

## Checkpoint

Start the MCP server with **no** `BP_READER_BACKEND` env var set.
Confirm it picks `auto`:

```
[bp-reader-mcp] backend = auto (uproject auto-discovered)
```

With the editor **closed**, call any tool. The probe finds no
handshake file → routes to commandlet → spawns the daemon. First
call eats cold-start; subsequent calls are tens of ms (Chapter 9).

Without restarting the MCP server, open the editor. Wait 2 – 5
seconds for the listener to come up and write the handshake file.
Call any tool again. The probe finds the handshake file, the TCP
probe succeeds, and the call routes to live. `Get-Process
UnrealEditor-Cmd` confirms the commandlet daemon is still running
(we don't kill it; we just stop routing to it) and the live op
completed without spawning anything new.

Close the editor. The next call should drop back to commandlet
(within the 2-second cache window). Tail the MCP server's stderr
during this transition; you should see the probe transition logged.

Set `BP_READER_BACKEND=commandlet` and re-run. Every call routes to
commandlet, even with the editor open. Set
`BP_READER_BACKEND=live` and re-run with the editor closed; every
call should fail with the "is the editor running" diagnostic from
Chapter 10. This proves the explicit override beats the auto policy
in both directions.

Finally: in a session where the editor was always open, run
`shutdown_daemon`. The response should be:

```json
{
  "ok": true,
  "was_running": false,
  "hint": "auto backend never spawned a commandlet (live editor covered every call this session)"
}
```

That's the auto backend telling you it was efficient enough to
never spin up a commandlet at all.

You now have transparent backend selection. Chapter 12 zooms out to
a different concern: how the MCP server represents BP-to-source
translation so that adding a target language (C++, Lua, etc.) is a
matter of plugging in a codegen module, not rewriting the
introspector.

See also: [design/05-backends.md](../design/05-backends.md) for the
full backend lifecycle and probe state machine.
