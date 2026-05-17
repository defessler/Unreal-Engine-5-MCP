#include "BatchContext.h"

#include "Engine/Blueprint.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace BlueprintReader
{
	namespace
	{
		// Thread-local — set by FConnectionScope before each dispatch.
		// thread_local is the right scope: each server thread + the game
		// thread that runs the AsyncTask body each get their own slot.
		// In practice the AsyncTask body runs on the game thread, so the
		// game-thread TLS is what the dispatch helpers read.
		thread_local uint64 GCurrentConnectionId = 0;
	}

	FBatchRegistry& FBatchRegistry::Get()
	{
		// Function-local static: thread-safe init under C++11; lifetime
		// matches the editor process. Daemon process exits = registry
		// gone, which is the right semantics (no persistence across runs).
		static FBatchRegistry Instance;
		return Instance;
	}

	FBatchContext* FBatchRegistry::Find(uint64 ConnectionId)
	{
		FScopeLock Lock(&Mu);
		if (TUniquePtr<FBatchContext>* P = Contexts.Find(ConnectionId))
		{
			return P->Get();
		}
		return nullptr;
	}

	FBatchContext& FBatchRegistry::GetOrCreate(uint64 ConnectionId)
	{
		// ConnectionId == 0 sentinel: legacy single-session callers
		// (direct -run=BlueprintReader without socket). They get a real
		// context too — just sharing slot 0. Backwards compatible.
		FScopeLock Lock(&Mu);
		TUniquePtr<FBatchContext>& P = Contexts.FindOrAdd(ConnectionId);
		if (!P.IsValid())
		{
			P = MakeUnique<FBatchContext>();
		}
		// Pointer stability: TMap may rehash on FindOrAdd, but the
		// TUniquePtr's owned object stays at a fixed address — safe to
		// return a reference to it after we drop the lock.
		return *P;
	}

	int32 FBatchRegistry::AcquireWriteOwnership(uint64 ConnectionId, UBlueprint* BP, uint64& OutHeldByConnectionId)
	{
		OutHeldByConnectionId = 0;
		if (ConnectionId == 0 || !BP)
		{
			return 0;
		}
		FScopeLock Lock(&Mu);
		TWeakObjectPtr<UBlueprint> Key(BP);
		if (uint64* Holder = WriteOwners.Find(Key))
		{
			if (*Holder == ConnectionId)
			{
				return 0;  // already mine
			}
			OutHeldByConnectionId = *Holder;
			return 1;  // locked by other
		}
		WriteOwners.Add(Key, ConnectionId);
		return 0;
	}

	void FBatchRegistry::ReleaseAllWriteOwnership(uint64 ConnectionId)
	{
		if (ConnectionId == 0)
		{
			return;
		}
		FScopeLock Lock(&Mu);
		for (auto It = WriteOwners.CreateIterator(); It; ++It)
		{
			if (It.Value() == ConnectionId)
			{
				It.RemoveCurrent();
			}
		}
	}

	TArray<TWeakObjectPtr<UBlueprint>> FBatchRegistry::Discard(uint64 ConnectionId)
	{
		TArray<TWeakObjectPtr<UBlueprint>> OutPending;
		if (ConnectionId == 0)
		{
			return OutPending;
		}
		FScopeLock Lock(&Mu);
		if (TUniquePtr<FBatchContext>* P = Contexts.Find(ConnectionId))
		{
			if (P->IsValid())
			{
				OutPending = MoveTemp((*P)->Pending);
			}
		}
		Contexts.Remove(ConnectionId);
		// Same pass releases this conn's BP write ownership — disconnect
		// must not leave dangling owners blocking other sessions.
		for (auto It = WriteOwners.CreateIterator(); It; ++It)
		{
			if (It.Value() == ConnectionId)
			{
				It.RemoveCurrent();
			}
		}
		return OutPending;
	}

	FConnectionScope::FConnectionScope(uint64 ConnectionId)
		: Previous(GCurrentConnectionId)
	{
		GCurrentConnectionId = ConnectionId;
	}

	FConnectionScope::~FConnectionScope()
	{
		GCurrentConnectionId = Previous;
	}

	uint64 GetCurrentConnectionId()
	{
		return GCurrentConnectionId;
	}
}    // namespace BlueprintReader
