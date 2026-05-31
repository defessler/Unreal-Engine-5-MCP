// BlueprintReaderCmdletServer — in-commandlet TCP listener that lets the
// MCP server share a single `UnrealEditor-Cmd -Daemon` process across
// many client connections, mirroring the in-editor live server's wire
// protocol exactly.
//
// Why this exists: the original daemon used stdin/stdout to talk to a
// single MCP-server parent. That made multi-session usage (Claude Code,
// Claude Desktop, Copilot at the same time) require spawning N
// commandlet daemons against the same project — which fight over the
// asset registry / DDC / .uasset files and burn 1+ GB resident each.
// Hosting the daemon's dispatch behind the same TCP protocol the live
// server already speaks lets every MCP-server client connect to a
// single shared commandlet daemon process — same model as the editor
// live server, just for the headless commandlet path.
//
// Wire protocol (newline-delimited JSON frames over TCP, localhost):
//   server → client  { "type": "hello", "version": "1" }
//   client → server  { "type": "auth", "token": "<shared>" }
//   server → client  { "type": "auth_ok" } | { "type": "auth_fail" }
//   client → server  { "type": "op", "id": N, "args": ["-Op=...", ...] }
//   server → client  { "type": "result", "id": N, "code": K, "json": {...} }
//
// The `args` array is the same `-Op=Read -Asset=...` format the
// commandlet daemon's stdin loop accepts. We dispatch to the existing
// RunOneOp on the main commandlet thread and ship its EmitJson output
// back as `json`.
//
// Lifecycle: the listener is started ONLY from the `-Daemon` path of
// `UBPRCommandlet` — not from editor module startup. It
// picks an ephemeral port + random token and writes them to
// `<ProjectDir>/Saved/bp-reader-cmdlet.json` for the MCP server to
// discover. A sibling `<ProjectDir>/Saved/bp-reader-cmdlet.lock` file
// is held exclusively for the daemon's lifetime — a second daemon
// process attempting to start against the same project will fail to
// acquire it and refuse to launch, preventing two daemons from
// fighting over the same asset cache. Stops + cleans up both files
// on shutdown. Localhost-only — refuses any non-loopback connection.
//
// Handshake file shape:
//   { "version": 1, "host": "127.0.0.1", "port": <ephemeral>,
//     "token": "<32 hex chars>", "pid": <daemon pid>,
//     "started_at": "2026-05-09T..." }
//
// Auth: a 256-bit random token (two GUIDs concatenated) is the default;
// `BP_READER_CMDLET_TOKEN` env-var override is honored. The MCP server
// reads the token from the handshake file unless the env var is set
// explicitly. Anyone with read access to the file can talk to the
// listener — the file inherits the project directory's NTFS ACLs.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"             // FThreadSafeBool for bShuttingDown
#include "HAL/ThreadSafeCounter.h"          // FThreadSafeCounter for ActiveConnections
#include "HAL/ThreadSafeCounter64.h"        // FThreadSafeCounter64 for LastDisconnectAtUnix
#include "Interfaces/IPv4/IPv4Endpoint.h"  // FIPv4Endpoint used by accept callback

class FSocket;
class FTcpListener;
class ISocketSubsystem;

namespace BlueprintReader
{

class FCmdletServer
{
public:
	FCmdletServer();
	~FCmdletServer();

	// Starts the listener. Resolution order for the bind port:
	//   1. `Port` arg (non-zero)
	//   2. `BP_READER_CMDLET_PORT` env var (non-zero)
	//   3. ephemeral (kernel picks a free port)
	// Token resolution:
	//   1. `BP_READER_CMDLET_TOKEN` env var if set
	//   2. random 256-bit GUID-pair (default)
	// Returns true if the listener bound, the lifetime lock was
	// acquired, AND the handshake file was written. Returns false if
	// `BP_READER_CMDLET_DISABLED=1` is set, if a second daemon is
	// already running (lifetime lock contended), or if the bind failed
	// (port in use, perms). Failure is logged.
	bool Start(int32 Port = 0);

	// Tears down the listener, any active connection threads, and the
	// lifetime-lock + handshake files. Called from daemon shutdown.
	// Idempotent.
	void Stop();

	// Returns the actual port the listener bound to (useful if Start
	// was called with port 0 to mean "use env var" — caller can log it).
	int32 GetListenPort() const { return BoundPort; }

	// Shutdown signal for the daemon's main loop. The daemon spins on
	// this and exits when it flips true. Returns true when:
	//   1. `bShuttingDown` has been set explicitly (Stop / RequestShutdown
	//      / TerminateOnSignal — Ctrl-C handler etc.), or
	//   2. The daemon has been idle: zero active connections AND at least
	//      one client has connected+disconnected AND `IdleSeconds` have
	//      elapsed since the last disconnect. Configured via the
	//      `BP_READER_DAEMON_IDLE_SECONDS` env var (default 300s;
	//      minimum 5s to avoid flapping).
	bool WantsShutdown() const;

	// Async-signal-safe-ish shutdown request. ONLY flips bShuttingDown
	// so the daemon's polling loop in RunDaemon notices and exits.
	// Distinct from Stop() because Win32's console-control handler runs
	// on a dedicated handler thread the OS injected into the process,
	// and calling Stop() (which joins listener + connection threads)
	// from that handler thread risks deadlock if any of those threads
	// are currently blocked on a syscall the handler is also racing.
	// The polling loop in RunDaemon owns the main thread, so once
	// WantsShutdown() returns true it falls through to Stop() naturally
	// — that's where the real teardown happens.
	void RequestShutdown() { bShuttingDown = true; }

	// Best-effort signal-context cleanup. Called from UE's
	// ApplicationWillTerminateDelegate which fires inside UE's Win32
	// console-control handler just before it TerminateProcess()'s the
	// daemon. We CAN'T join threads here (no time), but we CAN
	// synchronously delete the handshake file and close the OS lock
	// handle so the next daemon launch on the same project sees a
	// clean slate.
	//
	// Distinct from Stop():
	//   - Stop() is the main-thread teardown path: joins listener +
	//     connection threads, then deletes files + releases lock.
	//   - TerminateOnSignal() is the handler-thread fast path: file
	//     deletes + lock release only, skips thread joins because
	//     TerminateProcess is about to kill them anyway.
	//
	// Idempotent and safe to call after Stop(). Sets bShuttingDown
	// too so any code racing it sees the shutdown flag.
	void TerminateOnSignal();

private:
	// FTcpListener accept callback. Spawns a per-connection worker
	// thread; returns true so the listener keeps the connection (the
	// worker takes ownership of the socket).
	bool OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint);

	// Write `<ProjectDir>/Saved/bp-reader-cmdlet.json` so the MCP server
	// can discover port + token without the user having to plumb env
	// vars through their MCP client config. Idempotent (overwrites any
	// stale file from a prior crashed daemon). Returns true on success;
	// logs a warning and returns false on I/O failure.
	bool WriteHandshakeFile();

	// Delete the handshake file written by WriteHandshakeFile. Called
	// from Stop(). Best-effort — missing file is fine.
	void DeleteHandshakeFile();

	// Path the handshake file is written to. Encapsulated so tests +
	// the delete path agree.
	static FString HandshakeFilePath();

	// Persistent "preferred port" cache. Survives daemon shutdown so
	// the next launch can try the same port the previous run bound.
	// Stays at `<Project>/Saved/bp-reader-cmdlet-port.json` — separate
	// from the handshake file (which IS deleted on shutdown so MCP
	// probes fail fast against a dead daemon). Implementation in
	// BlueprintReaderCmdletServer.cpp.
	static FString PortCacheFilePath();
	static int32   ReadCachedPort();
	static void    WriteCachedPort(int32 Port);

	// Path of the lifetime-lock file. Held exclusively for the
	// daemon's lifetime; competing daemon launches fail at Start.
	static FString LifetimeLockFilePath();

	// Acquire an OS-level exclusive lock on the lifetime-lock file
	// (Windows-only today; non-Windows fallback is a presence check —
	// see TODO in the .cpp.).
	// On Windows we use raw `CreateFileW(dwShareMode=0)` because UE's
	// `FArchive` writers do NOT hold OS-level exclusive locks — a
	// second daemon could still open them and the contention probe
	// would miss. Returns true if the lock was acquired (stored in
	// `LifetimeLockHandle`), false if it was already held by another
	// process.
	bool AcquireLifetimeLock();

	// Release + delete the lifetime-lock file. Idempotent.
	void ReleaseLifetimeLock();

	TUniquePtr<FTcpListener> Listener;
	// FSocket pre-bound (so port 0 → kernel-picks-port works AND we can
	// see the actual port). Listener takes a reference; we own the
	// lifetime and destroy in Stop(). Null when Listener is null.
	FSocket* ListenerSocket = nullptr;
	FString ExpectedToken;        // env-var override OR random GUID
	int32 BoundPort = 0;
	bool  HandshakeWritten = false;
	// Atomic so the polling loop in RunDaemon's `while (!WantsShutdown())`
	// can't be hoisted by LTO. Matches FLiveServer::bStopRequested.
	FThreadSafeBool bShuttingDown = false;

	// OS-level handle to the lifetime-lock file. On Windows this is a
	// `HANDLE` from `CreateFileW`; stored as `void*` to keep this
	// header free of `<windows.h>`. Null when the lock is not held.
	void* LifetimeLockHandle = nullptr;

	// Idle-shutdown bookkeeping. The accept callback increments
	// `ActiveConnections`; the per-connection runnable decrements it +
	// stamps `LastDisconnectAtUnix` when its Run() returns. When the
	// active count is zero and `IdleSeconds` have elapsed since the
	// last disconnect, `WantsShutdown()` returns true so the daemon
	// exits gracefully. Env var `BP_READER_DAEMON_IDLE_SECONDS`
	// overrides the default 300s (5 min); values below 5s are rejected.
	//
	// `LastDisconnectAtUnix == 0` is the "no client has ever connected"
	// sentinel — keeps the daemon alive indefinitely on startup so the
	// first MCP-server probe always finds it. Stamp uses
	// `FDateTime::UtcNow().ToUnixTimestamp()`.
	FThreadSafeCounter   ActiveConnections;
	FThreadSafeCounter64 LastDisconnectAtUnix;
	int32                IdleSeconds = 300;

	// Hard max-lifetime backstop (defense-in-depth). Idle-shutdown can't
	// fire if a client connection wedges open (ActiveConnections never
	// returns to 0); this caps total wall-clock lifetime regardless of
	// activity. Off by default (0). Env BP_READER_DAEMON_MAX_LIFETIME_SECONDS
	// enables it; StartedAtUnix is stamped in Start().
	int32                MaxLifetimeSeconds = 0;
	int64                StartedAtUnix      = 0;

	// Monotonic counter handing each accepted socket a unique
	// ConnectionId. Used by FBatchRegistry to key per-session state so
	// two clients can run concurrent BeginBatch/EndBatch cycles without
	// clobbering each other. 0 is reserved for "no session" (legacy
	// direct -run= callers); increments start at 1.
	FThreadSafeCounter64 NextConnectionId;

public:
	// Allocate the next ConnectionId. Called once per accepted socket
	// in OnIncomingConnection.
	uint64 AllocateConnectionId();

	// Called by the per-connection runnable when its Run() returns
	// (client disconnected). Flushes any pending batched edits (Task
	// 4.4 commit-partial), discards the FBatchRegistry slot, decrements
	// the active-connections counter, and stamps `LastDisconnectAtUnix`
	// if the counter hit zero so the idle window starts ticking. Public
	// so the runnable in the .cpp's anonymous namespace can reach it
	// through a back-pointer. Pass 0 for the early-failure path where
	// no connection id was ever allocated (e.g. thread spawn failed).
	void OnClientDisconnected(uint64 ConnectionId = 0);
};

// Module-level singleton accessor. The daemon owns one instance for
// its lifetime; tests + other code can reach it via this if needed.
FCmdletServer* GetCmdletServer();

}    // namespace BlueprintReader
