// BlueprintReaderLogSink — module-level ring buffer that captures the
// editor's output log so the MCP `read_output_log` tool can drain
// recent entries on demand.
//
// Why a ring buffer + custom FOutputDevice: UE's log device chain
// (GLog) doesn't expose "give me the last N messages" anywhere. The
// editor's Output Log panel scrolls a UI-side buffer; commandlet
// stdout is line-oriented and lossy by the time we'd parse it.
// Registering our own FOutputDevice into GLog gives us every Serialize
// call as it happens, which we fold into a fixed-capacity ring.
//
// Sized at 1024 entries by default — enough to hold a couple minutes
// of normal editor logging, small enough that 1000+ verbose messages
// from a noisy plugin won't blow memory. Capacity is overridable via
// BP_READER_LOG_BUFFER env var.
//
// Thread-safety: GLog can fire Serialize from any thread (audio,
// loading threads, etc.). Drain() runs on the game thread (via
// AsyncTask) from the live server's worker. A FCriticalSection guards
// the buffer.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "HAL/CriticalSection.h"

namespace BlueprintReader
{

struct FLogSinkEntry
{
    FDateTime Timestamp;       // UTC capture time
    ELogVerbosity::Type Verbosity = ELogVerbosity::NoLogging;
    FName Category;
    FString Message;
};

// Custom output device. Owned by the module singleton; registered with
// GLog at StartupModule, unregistered at Shutdown.
class FLogSink : public FOutputDevice
{
public:
    explicit FLogSink(int32 InCapacity = 1024);
    virtual ~FLogSink() override;

    // FOutputDevice — runs on whatever thread emitted the log message.
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                            const FName& Category) override;

    // Drain up to `MaxEntries` of the most recent log lines, optionally
    // filtered by minimum verbosity (`Fatal`/`Error`/`Warning`/`Display`/
    // `Log` — NoLogging accepts everything). Returns newest-last order.
    void Drain(int32 MaxEntries,
                ELogVerbosity::Type MinVerbosity,
                TArray<FLogSinkEntry>& Out);

private:
    FCriticalSection Mu;
    TArray<FLogSinkEntry> Ring;   // sized to Capacity, wraparound via Head
    int32 Head = 0;               // index where the next entry writes
    int32 Count = 0;              // entries currently stored (≤ Capacity)
    int32 Capacity = 0;
};

// Module-singleton accessor. Returns nullptr before StartupModule and
// after Shutdown (Drain callers should null-check + return empty).
FLogSink* GetLogSink();

// Lifecycle hooks called from the editor module.
void StartLogSink();
void StopLogSink();

}  // namespace BlueprintReader
