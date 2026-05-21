// Pluggable analytics provider — Phase 7. The default is a no-op
// (zero observability) so privacy stays opt-in. Users who want
// telemetry inject a real provider implementation at startup.
//
// Events:
//   OnSessionStart  — fired once when RegisterHandlers wires things
//   OnSessionEnd    — fired once when the Logger object is destroyed
//   OnToolCall      — fired per tools/call invocation, after the
//                     tool returned (or threw). Carries the tool name,
//                     elapsed ms, and an isError flag.
//
// Tool names are passed RAW. Providers that want privacy can hash
// them before forwarding to their backing telemetry system; we don't
// hash at this layer because hashing is a provider concern (different
// telemetry systems use different hash algorithms, salts, etc.) and
// the default no-op does nothing with the names anyway.
//
// Thread-safety: the provider's methods may be called from any thread
// the future async dispatch path uses. Implementations must be
// internally synchronized. The no-op default trivially satisfies this.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace bpr::tools {

struct ToolCallSample {
	std::string_view tool;       // raw tool name (e.g. "read_blueprint")
	std::chrono::milliseconds elapsed;
	bool        isError;         // tool threw (envelope had isError:true)
};

class IAnalyticsProvider {
public:
	virtual ~IAnalyticsProvider() = default;

	virtual void OnSessionStart() = 0;
	virtual void OnSessionEnd() = 0;
	virtual void OnToolCall(const ToolCallSample& sample) = 0;
};

// Builds a default no-op provider. Use this when BP_READER_ANALYTICS is
// off (the default) — every method short-circuits.
std::unique_ptr<IAnalyticsProvider> MakeNoOpAnalyticsProvider();

// Returns true when the env opted in via BP_READER_ANALYTICS=1
// (or true/yes/on). Used by main.cpp to decide whether to wire a
// real provider. Default off — privacy-preserving default.
bool AnalyticsEnabled();

// Returns the EULA / privacy notice text that gets logged once at
// startup. Mentions the analytics gate explicitly so the user knows
// what they're opting into.
std::string EulaNotice();

}    // namespace bpr::tools
