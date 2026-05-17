// BPRoundtripModule — module entry point.
//
// This module is the compile target for BP-to-C++ source the BPIR
// roundtrip pipeline (BPIRRoundtrip in the MCP test tree) emits at
// test time. Production code is empty; generated code lands in
// Private/Generated/ (gitignored) when the roundtrip tests run.
//
// The module is Type=Runtime so the emitted UCLASS/UFUNCTION/UPROPERTY
// constructs stay runtime-compatible — i.e. the roundtrip exercises
// the same code shape a hand-written game module would have.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogBPRoundtripModule, Log, All);

IMPLEMENT_MODULE(FDefaultModuleImpl, BPRoundtripModule);
