#include "BlueprintReaderLogSink.h"

#include "HAL/PlatformMisc.h"
#include "Misc/OutputDeviceRedirector.h"

namespace BlueprintReader
{

namespace
{
    TUniquePtr<FLogSink> GLogSink;

    int32 ResolveCapacityFromEnv()
    {
        // Optional override for users with very chatty editor sessions.
        FString S = FPlatformMisc::GetEnvironmentVariable(TEXT("BP_READER_LOG_BUFFER"));
        if (!S.IsEmpty())
        {
            const int32 N = FCString::Atoi(*S);
            if (N > 0 && N <= 1'000'000) return N;
        }
        return 1024;
    }
}

FLogSink::FLogSink(int32 InCapacity)
    : Capacity(InCapacity)
{
    Ring.SetNum(Capacity);
}

FLogSink::~FLogSink()
{
    // GLog removal is handled by StopLogSink; just clear the buffer.
    FScopeLock Lock(&Mu);
    Ring.Empty();
    Head = 0;
    Count = 0;
}

void FLogSink::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                          const FName& Category)
{
    if (Capacity <= 0 || !V) return;

    FLogSinkEntry Entry;
    Entry.Timestamp = FDateTime::UtcNow();
    Entry.Verbosity = Verbosity;
    Entry.Category  = Category;
    Entry.Message   = V;

    FScopeLock Lock(&Mu);
    Ring[Head] = MoveTemp(Entry);
    Head = (Head + 1) % Capacity;
    if (Count < Capacity) ++Count;
}

namespace
{
    // Verbosity ordering: Fatal (1) > Error (2) > Warning (3) > Display
    // (4) > Log (5) > Verbose (6) > VeryVerbose (7). Lower number = more
    // severe.
    bool VerbosityPasses(ELogVerbosity::Type EntryVerbosity,
                         ELogVerbosity::Type MinVerbosity)
    {
        if (MinVerbosity == ELogVerbosity::NoLogging) return true;
        return static_cast<uint8>(EntryVerbosity) <= static_cast<uint8>(MinVerbosity);
    }
}

void FLogSink::Drain(int32 MaxEntries,
                     ELogVerbosity::Type MinVerbosity,
                     TArray<FLogSinkEntry>& Out)
{
    Out.Reset();
    FScopeLock Lock(&Mu);
    if (Count == 0 || MaxEntries <= 0) return;

    // Walk the ring oldest→newest, accumulating filtered hits. We
    // collect into a temp buffer, then slice to MaxEntries from the
    // tail (the most recent matches).
    TArray<FLogSinkEntry> Filtered;
    Filtered.Reserve(Count);
    const int32 Tail = (Head - Count + Capacity) % Capacity;
    for (int32 i = 0; i < Count; ++i)
    {
        const FLogSinkEntry& E = Ring[(Tail + i) % Capacity];
        if (VerbosityPasses(E.Verbosity, MinVerbosity)) Filtered.Add(E);
    }
    const int32 Start = FMath::Max(0, Filtered.Num() - MaxEntries);
    for (int32 i = Start; i < Filtered.Num(); ++i)
    {
        Out.Add(Filtered[i]);
    }
}

FLogSink* GetLogSink() { return GLogSink.Get(); }

void StartLogSink()
{
    if (GLogSink) return;
    GLogSink = MakeUnique<FLogSink>(ResolveCapacityFromEnv());
    // GLog dispatches to every registered output device.
    if (GLog)
    {
        GLog->AddOutputDevice(GLogSink.Get());
    }
}

void StopLogSink()
{
    if (!GLogSink) return;
    if (GLog)
    {
        GLog->RemoveOutputDevice(GLogSink.Get());
    }
    GLogSink.Reset();
}

}  // namespace BlueprintReader
