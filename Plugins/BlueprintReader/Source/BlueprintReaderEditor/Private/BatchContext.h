// BatchContext — per-connection batch state for the shared daemon.
//
// History: the daemon used file-scope statics for `BatchDeferFlag` and
// `BatchPending`. That works for a single-session daemon, but in the
// multi-session topology (PR #68) two sessions hitting the same daemon
// stomp each other:
//
//   conn1: BeginBatch              # static defer = true
//   conn1: add_node A to BP_X      # static pending = [BP_X]
//   conn2: BeginBatch              # static pending RESET — conn1's edit pending lost
//   conn2: EndBatch                # compiles nothing — conn1's BP_X never saved
//   conn1: EndBatch                # static pending is empty — silent no-op
//
// Per-connection contexts fix the data-race. Per-BP write ownership also
// blocks the "two sessions edit same BP mid-batch" cross-corruption case
// with a clean error response instead of an interleaved write.
//
// Threading note: dispatch runs on the game thread (one frame at a time —
// `AsyncTask(ENamedThreads::GameThread, ...)` from each server thread,
// awaited by that thread). So a single thread-local `CurrentConnectionId`
// is enough to identify the active connection inside RunOneOp. Each
// server installs a scope at dispatch-entry and clears it at exit.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UBlueprint;

namespace BlueprintReader
{
	/** One entry per live socket connection. Reset on disconnect. */
	struct FBatchContext
	{
		bool bDeferCompile = false;
		TArray<TWeakObjectPtr<UBlueprint>> Pending;
	};

	/**
	 * Owns per-connection batch state + BP write-ownership map.
	 *
	 * The write-ownership map answers: "while conn1 has BP_X in a pending
	 * batch, is conn2 allowed to mutate BP_X?" Today the answer is NO —
	 * we surface a `blueprint_locked_by_other_session` error so conn2 can
	 * retry instead of silently overwriting conn1's pending state on disk.
	 *
	 * All public methods take their own internal lock — safe to call from
	 * any thread (the disconnect hook lands on a server thread, every
	 * other call lands on the game thread).
	 */
	class FBatchRegistry
	{
	public:
		static FBatchRegistry& Get();

		/** Returns nullptr only if ConnectionId == 0 (legacy / no-session). */
		FBatchContext* Find(uint64 ConnectionId);

		/** Creates the context on first touch. Always non-null for non-zero ConnectionId. */
		FBatchContext& GetOrCreate(uint64 ConnectionId);

		/**
		 * Mark BP as owned by `ConnectionId` for the duration of an open batch.
		 * Returns: 0 = acquired (or already held), 1 = locked-by-other (held_by set).
		 * No-op (returns 0) for ConnectionId == 0.
		 */
		int32 AcquireWriteOwnership(uint64 ConnectionId, UBlueprint* BP, uint64& OutHeldByConnectionId);

		/** Release every BP this connection owns. Called by EndBatch and disconnect. */
		void ReleaseAllWriteOwnership(uint64 ConnectionId);

		/**
		 * Disconnect hook. Returns the pending BP set so the caller can
		 * commit-partial (compile + save) before the context is wiped.
		 *
		 * After this returns, calling `Find/GetOrCreate(ConnectionId)`
		 * for the same id reallocates a fresh context — id reuse is OK
		 * (the daemon's counter is monotonic, but defense in depth).
		 */
		TArray<TWeakObjectPtr<UBlueprint>> Discard(uint64 ConnectionId);

	private:
		FCriticalSection Mu;
		// TUniquePtr so the FBatchContext stays at a stable address even
		// if the TMap rehashes — references handed out by GetOrCreate
		// remain valid until Discard() destroys them. Disconnect lands
		// on a server thread that has already joined its own AsyncTask
		// queue, so by construction no game-thread reader is still
		// dereferencing the slot when Discard runs.
		TMap<uint64, TUniquePtr<FBatchContext>> Contexts;
		// BP→holder. Single owner per BP; we don't queue waiters — the
		// game-thread dispatcher can't block without deadlocking itself.
		TMap<TWeakObjectPtr<UBlueprint>, uint64> WriteOwners;
	};

	/**
	 * RAII scope: set the thread-local current connection id for the
	 * duration of one op dispatch. Servers wrap the AsyncTask body with
	 * this — every RunOneOp call sees the right id.
	 */
	class FConnectionScope
	{
	public:
		explicit FConnectionScope(uint64 ConnectionId);
		~FConnectionScope();
		FConnectionScope(const FConnectionScope&) = delete;
		FConnectionScope& operator=(const FConnectionScope&) = delete;
	private:
		uint64 Previous = 0;
	};

	/** Returns 0 if no scope is active (legacy / non-server callers). */
	uint64 GetCurrentConnectionId();

	/**
	 * Commit-partial-on-disconnect (Task 4.4).
	 * Called from the server's disconnect path. Synchronously schedules
	 * a game-thread compile + save of every BP the connection had
	 * pending. Defined in BlueprintReaderCommandlet.cpp (which owns
	 * CompileAndSaveBlueprint); declared here to break the include
	 * cycle. Honors `BP_READER_BATCH_ON_DISCONNECT=discard` to drop
	 * pending edits instead — matches the "fail closed" mode where the
	 * client wants no half-applied state.
	 */
	void FlushBatchForConnection(uint64 ConnectionId);
}    // namespace BlueprintReader
