// TEST-2 P1a — the modal side-channel.
//
// The normal op dispatch marshals work to the game thread via
// AsyncTask(ENamedThreads::GameThread) and blocks the connection's worker
// thread on an FEvent until it runs. A *hard modal* (FSlateApplication::
// AddModalWindow) spins its own nested loop on the game thread that ticks the
// OS pump + Slate draw but NOT FTSTicker / the task graph — so that marshalled
// op never runs and the dispatch wedges for as long as the modal is up.
//
// This channel routes around that. A `modal` TCP frame is answered ON THE
// WORKER THREAD by SubmitModalCommand, which only enqueues a command and waits
// on an event; the actual game-thread work runs in a drainer invoked from two
// places: the heartbeat FTSTicker (when the editor is idle, no modal) and the
// FSlateApplication OnModalLoopTickEvent delegate (the one game-thread context
// that DOES run inside the modal pump). Either drains the queue, so the editor
// is reportable — and recoverable (dismiss) — even while a modal blocks it.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintReader
{
	// Worker thread: enqueue a modal command and block until a game-thread
	// drainer services it (or TimeoutMs elapses). Actions: "report" (default —
	// {is_open, title, buttons[], buttons_truncated}), "dismiss" (report +
	// RequestDestroyWindow on the active modal, adds {dismissed}). Always returns
	// a non-null object; a timeout yields {serviced:false, note}. ButtonPath is
	// reserved for a future "click" action (P1b).
	TSharedPtr<FJsonObject> SubmitModalCommand(const FString& Action,
											   const FString& ButtonPath,
											   int32 TimeoutMs);

	// Game thread ONLY: execute every queued modal command now. Cheap no-op on an
	// empty queue. Called from the heartbeat ticker and the modal-loop delegate.
	void DrainModalCommands();

	// Game thread: register the OnModalLoopTickEvent drainer once Slate is up.
	// Idempotent and a no-op until FSlateApplication::IsInitialized(); call it
	// each heartbeat tick (a single bool check after the first success).
	void RegisterModalTickIfNeeded();

	// Game thread: remove the OnModalLoopTickEvent registration (module shutdown).
	void UnregisterModalTick();

	// Game thread (module shutdown, BEFORE the servers' worker threads are
	// joined): release every queued command with a {serviced:false, note:
	// "editor shutting down"} result so a worker blocked in SubmitModalCommand's
	// wait returns immediately instead of stalling the join up to its timeout.
	void FailPendingModalCommands();
}
