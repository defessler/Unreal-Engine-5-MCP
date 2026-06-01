#include "DaemonProgress.h"

#include "BatchContext.h"   // GetCurrentConnectionId
#include "Misc/FeedbackContext.h"

namespace BlueprintReader
{
	namespace
	{
		FCriticalSection GProgressMu;
		TMap<uint64, TArray<FDaemonProgressEntry>> GQueues;   // ConnId -> entries

		// Forwarding feedback context: capture ProgressReported into the
		// connection's queue, delegate everything else (logging, etc.) to the
		// real GWarn that was active when we installed. Only Serialize is pure
		// on FFeedbackContext; the rest default through it.
		class FProgressForwardingContext : public FFeedbackContext
		{
		public:
			FProgressForwardingContext(FFeedbackContext* InInner, uint64 InConnId)
				: Inner(InInner), ConnId(InConnId) {}

			void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
			{
				if (Inner) { Inner->Serialize(V, Verbosity, Category); }
			}

			void ProgressReported(const float TotalProgressInterp, FText DisplayMessage) override
			{
				// TotalProgressInterp is 0..1; report as a fraction (Total=1).
				EmitConnectionProgress(static_cast<double>(TotalProgressInterp), 1.0,
									   DisplayMessage.ToString());
				// Preserve the base behavior (broadcasts ApplicationHeartbeat so
				// long ops don't trip the hang detector). Inner->ProgressReported
				// is protected on another instance, so we can't delegate to it;
				// the base call is equivalent for the headless daemon.
				FFeedbackContext::ProgressReported(TotalProgressInterp, DisplayMessage);
			}

			FFeedbackContext* Inner = nullptr;
			uint64            ConnId = 0;
		};
	}

	void RegisterProgressQueue(uint64 ConnectionId)
	{
		if (ConnectionId == 0) { return; }
		FScopeLock Lock(&GProgressMu);
		GQueues.FindOrAdd(ConnectionId);
	}

	void UnregisterProgressQueue(uint64 ConnectionId)
	{
		if (ConnectionId == 0) { return; }
		FScopeLock Lock(&GProgressMu);
		GQueues.Remove(ConnectionId);
	}

	TArray<FDaemonProgressEntry> DrainProgress(uint64 ConnectionId)
	{
		if (ConnectionId == 0) { return {}; }
		FScopeLock Lock(&GProgressMu);
		if (TArray<FDaemonProgressEntry>* Q = GQueues.Find(ConnectionId))
		{
			TArray<FDaemonProgressEntry> Out = MoveTemp(*Q);
			Q->Reset();
			return Out;
		}
		return {};
	}

	void EmitConnectionProgress(double Current, double Total, const FString& Message)
	{
		const uint64 ConnId = GetCurrentConnectionId();
		if (ConnId == 0) { return; }
		FScopeLock Lock(&GProgressMu);
		if (TArray<FDaemonProgressEntry>* Q = GQueues.Find(ConnId))
		{
			Q->Add(FDaemonProgressEntry{ Current, Total, Message });
		}
	}

	FScopedProgressCapture::FScopedProgressCapture(uint64 InConnId)
		: ConnectionId(InConnId)
	{
		if (ConnectionId == 0) { return; }
		Previous = GWarn;
		Forwarder = MakeUnique<FProgressForwardingContext>(Previous, ConnectionId);
		GWarn = Forwarder.Get();
	}

	FScopedProgressCapture::~FScopedProgressCapture()
	{
		if (ConnectionId == 0) { return; }
		// Restore only if we're still the active context (defensive against a
		// nested swap that didn't restore — shouldn't happen, ops are serial).
		if (GWarn == Forwarder.Get())
		{
			GWarn = Previous;
		}
	}
}
