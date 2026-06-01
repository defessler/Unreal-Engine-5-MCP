// Daemon-side progress emission for long ops.
//
// The cmdlet/live server dispatches each op on the game thread and the
// connection thread blocks awaiting it. To report progress mid-op we:
//   * install FScopedProgressCapture around the dispatch (swaps GWarn for a
//     forwarding FFeedbackContext that turns FScopedSlowTask progress into
//     EmitConnectionProgress on the game thread), and
//   * have the connection thread poll-drain the per-connection queue while it
//     waits, sending {"type":"progress"} frames before the result.
//
// The queue is keyed by connection id (BatchContext's GetCurrentConnectionId)
// and guarded by a mutex — the game thread pushes, the connection thread drains.
#pragma once

#include "CoreMinimal.h"

namespace BlueprintReader
{
	struct FDaemonProgressEntry
	{
		double  Current = 0.0;   // 0..Total (or 0..1 when Total<=0 / unknown)
		double  Total   = 0.0;   // <=0 means unknown-duration
		FString Message;
	};

	// Connection-thread: ensure/remove a connection's progress queue. Safe to
	// call with id 0 (no-op).
	void RegisterProgressQueue(uint64 ConnectionId);
	void UnregisterProgressQueue(uint64 ConnectionId);

	// Connection-thread: pop everything queued for this connection (empty if none).
	TArray<FDaemonProgressEntry> DrainProgress(uint64 ConnectionId);

	// Game-thread: queue a progress entry for the current connection
	// (BatchContext::GetCurrentConnectionId). No-op when no connection scope is
	// active or its queue isn't registered.
	void EmitConnectionProgress(double Current, double Total, const FString& Message);

	// RAII: for the duration of one op dispatch, swap GWarn for a forwarding
	// feedback context so FScopedSlowTask progress (cook/package/lighting/
	// compile/save all use them) becomes EmitConnectionProgress for this
	// connection. Restores GWarn on scope exit. No-op when ConnectionId == 0.
	class FScopedProgressCapture
	{
	public:
		explicit FScopedProgressCapture(uint64 ConnectionId);
		~FScopedProgressCapture();
		FScopedProgressCapture(const FScopedProgressCapture&) = delete;
		FScopedProgressCapture& operator=(const FScopedProgressCapture&) = delete;
	private:
		uint64                       ConnectionId = 0;
		class FFeedbackContext*      Previous = nullptr;
		TUniquePtr<class FFeedbackContext> Forwarder;
	};
}
