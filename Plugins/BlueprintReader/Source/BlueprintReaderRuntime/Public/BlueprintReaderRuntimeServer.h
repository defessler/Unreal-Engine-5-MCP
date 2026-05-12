// BlueprintReaderRuntimeServer — opt-in TCP listener that lets the MCP
// server talk to a packaged-game runtime with the same wire protocol
// the editor-side live server speaks.
//
// Defaults to OFF: a shipping game should NOT open a port silently.
// Two opt-in mechanisms:
//   * CVar `bp.reader.listen 1` (game console or DefaultGame.ini)
//   * Env var `BP_READER_RUNTIME_LISTEN=1` (process-launch override)
//
// Wire protocol — identical to BlueprintReaderLiveServer so the MCP
// server's existing `live` backend works against either target:
//   server → client  { "type": "hello", "version": "1" }
//   client → server  { "type": "auth", "token": "<shared>" }
//   server → client  { "type": "auth_ok" } | { "type": "auth_fail" }
//   client → server  { "type": "op", "id": N, "args": ["-Op=...", ...] }
//   server → client  { "type": "result", "id": N, "code": K, "json": {...} }
//
// Supported ops (subset of the full editor surface — what UClass
// reflection can answer in a cooked build):
//   -Op=List       [-Path=/Game/...]   → ListBlueprints
//   -Op=Read       -Asset=/Game/...    → full BP introspection
//   -Op=Variables  -Asset=/Game/...    → variables[] subset
//   -Op=Components -Asset=/Game/...    → components[] subset
//   -Op=Functions  -Asset=/Game/...    → functions[] subset (signatures)
//
// Anything else returns `{type:"result", code: -1, json: {"error": ...}}`
// so the client knows to fall back to the editor backend if available.
//
// Handshake file at `<ProjectDir>/Saved/bp-reader-runtime.json` (note
// the `-runtime` suffix — distinct from the editor's
// `bp-reader-live.json` so a packaged game running alongside the
// editor doesn't collide). Same JSON shape as the editor side so the
// MCP server's discovery code is unchanged.
//
// Loopback-only. Refuses non-127.0.0.1 connections.
#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

class FSocket;
class FTcpListener;

namespace BlueprintReaderRuntime
{

class BLUEPRINTREADERRUNTIME_API FRuntimeServer
{
public:
	FRuntimeServer();
	~FRuntimeServer();

	// Start listening. Port resolution: explicit arg > env var
	// `BP_READER_RUNTIME_PORT` > 0 (kernel picks). Token: env var
	// `BP_READER_RUNTIME_TOKEN` > random 256-bit (two concatenated
	// GUIDs). Returns true on success; logs and returns false on bind
	// failure / disabled-by-env. Idempotent — second call returns
	// true without reinitialising.
	bool Start(int32 Port = 0);

	// Tear down. Joins worker threads, closes socket, deletes handshake
	// file. Idempotent.
	void Stop();

	bool IsListening() const { return Listener.IsValid(); }
	int32 GetListenPort() const { return BoundPort; }

private:
	bool OnIncomingConnection(FSocket* Socket, const FIPv4Endpoint& Endpoint);

	bool WriteHandshakeFile();
	void DeleteHandshakeFile();
	static FString HandshakeFilePath();

	TUniquePtr<FTcpListener> Listener;
	FSocket* ListenerSocket = nullptr;
	FString ExpectedToken;
	int32 BoundPort = 0;
	bool HandshakeWritten = false;
};

// Module-level singleton. Module startup checks the CVar / env var
// and constructs+starts when opt-in is set.
FRuntimeServer* GetRuntimeServer();

} // namespace BlueprintReaderRuntime
