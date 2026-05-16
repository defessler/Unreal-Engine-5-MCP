// BlueprintReaderLiveServer — in-editor TCP listener that lets the MCP
// server talk to a running UE editor process directly, instead of
// spawning a second UnrealEditor-Cmd commandlet daemon.
//
// Why this exists: the user can have the full UE editor open AND the
// agent can read/mutate BPs through the MCP server, without two
// processes fighting over the same .uasset files / DDC / asset
// registry. Reads see the editor's live in-memory state (including
// unsaved changes); writes go through the editor's normal mutation
// pipeline so the content browser refreshes.
//
// Wire protocol (newline-delimited JSON frames over TCP, localhost):
//   server → client  { "type": "hello", "version": "1" }
//   client → server  { "type": "auth", "token": "<shared>" }
//   server → client  { "type": "auth_ok" } | { "type": "auth_fail" }
//   client → server  { "type": "op", "id": N, "args": ["-Op=...", ...] }
//   server → client  { "type": "result", "id": N, "code": K, "json": {...} }
//
// The `args` array is the same `-Op=Read -Asset=...` format the
// commandlet daemon accepts. We dispatch to the existing RunOneOp on
// the game thread and ship its EmitJson output back as `json`.
//
// Lifecycle: the listener starts at editor module init by default —
// it picks an ephemeral port + random token and writes them to
// `<ProjectDir>/Saved/bp-reader-live.json` for the MCP server to
// discover. Set `BP_READER_LIVE_DISABLED=1` to opt out entirely;
// `BP_READER_LIVE_PORT` and `BP_READER_LIVE_TOKEN` still override the
// auto-generated values. Stops + cleans up the handshake file at
// module shutdown. Localhost-only — refuses any non-loopback
// connection.
//
// Handshake file shape:
//   { "version": 1, "host": "127.0.0.1", "port": <ephemeral>,
//     "token": "<32 hex chars>", "pid": <editor pid>,
//     "started_at": "2026-05-09T..." }
//
// Auth: a 128-bit random token (FGuid) is the default; the env var
// override is still honored. The MCP server reads the token from the
// handshake file unless `BP_READER_LIVE_TOKEN` is set explicitly.
// Anyone with read access to the file can talk to the listener — the
// file inherits the project directory's NTFS ACLs, so don't widen
// project permissions if you don't want to widen this.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"  // FIPv4Endpoint used by accept callback

class FSocket;
class FTcpListener;
class ISocketSubsystem;

namespace BlueprintReader
{

class FLiveServer
{
public:
	FLiveServer();
	~FLiveServer();

	// Starts the listener. Resolution order for the bind port:
	//   1. `Port` arg (non-zero)
	//   2. `BP_READER_LIVE_PORT` env var (non-zero)
	//   3. ephemeral (kernel picks a free port)
	// Token resolution:
	//   1. `BP_READER_LIVE_TOKEN` env var if set
	//   2. random 128-bit GUID (default)
	// Returns true if the listener bound + the handshake file was
	// written. Returns false if `BP_READER_LIVE_DISABLED=1` is set, or
	// if the bind failed (port in use, perms). Failure is logged but
	// doesn't fail the editor module — daemon mode still works.
	bool Start(int32 Port = 0);

	// Tears down the listener and any active connection threads. Called
	// on module shutdown. Idempotent.
	void Stop();

	// Returns the actual port the listener bound to (useful if Start was
	// called with port 0 to mean "use env var" — caller can log it).
	int32 GetListenPort() const { return BoundPort; }

private:
	// FTcpListener accept callback. Spawns a per-connection worker
	// thread; returns true so the listener keeps the connection (the
	// worker takes ownership of the socket).
	bool OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint);

	// Write `<ProjectDir>/Saved/bp-reader-live.json` so the MCP server
	// can discover port + token without the user having to plumb env
	// vars through their MCP client config. Idempotent (overwrites any
	// stale file from a prior crashed editor session). Returns true on
	// success; logs a warning and returns false on I/O failure (the
	// listener still works for callers who already know the port +
	// token via env var).
	bool WriteHandshakeFile();

	// Delete the handshake file written by WriteHandshakeFile. Called
	// from Stop(). Best-effort — missing file is fine.
	void DeleteHandshakeFile();

	// Path the handshake file is written to. Encapsulated so tests +
	// the delete path agree.
	static FString HandshakeFilePath();

	// Persistent "preferred port" cache. Survives editor shutdown so
	// the next launch can try the same port the previous run bound.
	// Stays at `<Project>/Saved/bp-reader-live-port.json` — separate
	// from the handshake file (which IS deleted on shutdown so MCP
	// probes fail fast against a dead editor). Implementation in
	// BlueprintReaderLiveServer.cpp.
	static FString PortCacheFilePath();
	static int32   ReadCachedPort();
	static void    WriteCachedPort(int32 Port);

	TUniquePtr<FTcpListener> Listener;
	// FSocket pre-bound (so port 0 → kernel-picks-port works AND we can
	// see the actual port). Listener takes a reference; we own the
	// lifetime and destroy in Stop(). Null when Listener is null.
	FSocket* ListenerSocket = nullptr;
	FString ExpectedToken;        // env-var override OR random GUID
	int32 BoundPort = 0;
	bool  HandshakeWritten = false;
};

// Module-level singleton accessor. The editor module owns one instance
// for its lifetime; tests + other code can reach it via this if needed.
FLiveServer* GetLiveServer();

} // namespace BlueprintReader
