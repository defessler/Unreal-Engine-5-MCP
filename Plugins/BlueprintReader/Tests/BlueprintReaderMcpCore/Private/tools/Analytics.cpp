#include "tools/Analytics.h"

#include "Env.h"

#include <algorithm>
#include <cctype>
#include <memory>

namespace bpr::tools {

namespace analytics_detail {

class NoOpAnalyticsProvider : public IAnalyticsProvider {
public:
	void OnSessionStart() override {}
	void OnSessionEnd() override {}
	void OnToolCall(const ToolCallSample& /*sample*/) override {}
};

}    // namespace analytics_detail

std::unique_ptr<IAnalyticsProvider> MakeNoOpAnalyticsProvider() {
	return std::make_unique<analytics_detail::NoOpAnalyticsProvider>();
}

bool AnalyticsEnabled() {
	return env::IsTruthy(env::Get("BP_READER_ANALYTICS").value_or(""));
}

std::string EulaNotice() {
	return
		"bp-reader-mcp privacy notice:\n"
		"  - This MCP server runs LOCALLY and does not transmit any data to\n"
		"    third parties by default.\n"
		"  - Set BP_READER_ANALYTICS=1 to enable in-process analytics events\n"
		"    (SessionStart, SessionEnd, ToolCall). The default no-op provider\n"
		"    discards these events; only an explicit replacement provider\n"
		"    sends them anywhere.\n"
		"  - The BP_READER_VERBOSE=1 flag controls verbose stderr logging.\n"
		"    Off by default — the server is quiet unless something fails.\n"
		"  - The MCP `notifications/message` channel (Phase 6 logging) is a\n"
		"    SERVER->CLIENT stream; the client sees what their LLM session\n"
		"    asked for. Set BP_READER_LOG_LEVEL=off to suppress it.\n"
		"  - Tool calls operate on YOUR project files. Asset paths, BP\n"
		"    contents, and generated C++ flow back to your client session.\n"
		"    Review your client's data-handling policy if that matters.\n";
}

}    // namespace bpr::tools
