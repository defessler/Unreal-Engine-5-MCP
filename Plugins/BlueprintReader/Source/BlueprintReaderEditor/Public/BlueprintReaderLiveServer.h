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
// Lifecycle: the listener starts at editor module init (gated by env
// var BP_READER_LIVE_PORT — when 0 or unset, the listener is disabled
// and the editor module behaves exactly as before). Stops at module
// shutdown. Localhost-only — refuses any non-loopback connection.
//
// Auth: BP_READER_LIVE_TOKEN must be set in the editor process's env
// before launch. Connections that don't present the matching token in
// their `auth` frame are dropped before any op runs. Token is also
// the only thing protecting writes — anyone with localhost access who
// can read your env vars can mutate BPs.

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

    // Starts the listener on `Port` (or BP_READER_LIVE_PORT env var if
    // Port is 0). Returns true if the listener bound successfully. False
    // if disabled (port 0 / no env), or if the bind failed (port in use,
    // perms, etc.). Failure is logged but doesn't fail the editor module
    // — daemon mode still works as a fallback.
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

    TUniquePtr<FTcpListener> Listener;
    FString ExpectedToken;        // BP_READER_LIVE_TOKEN at module init
    int32 BoundPort = 0;
};

// Module-level singleton accessor. The editor module owns one instance
// for its lifetime; tests + other code can reach it via this if needed.
FLiveServer* GetLiveServer();

} // namespace BlueprintReader
