// Phase 7 tests: Analytics provider scaffolding + EULA notice + safety
// helpers. Smaller test set than other phases because the bulk of
// the work is scaffolding — Phase 7's actual telemetry implementation
// is deferred (no-op default ships; real provider is future work).

#include <doctest/doctest.h>

#include "tools/Analytics.h"

#include <chrono>
#include <memory>
#include <string>

using namespace bpr;

// =====================================================================
// No-op analytics provider
// =====================================================================

TEST_CASE("Analytics: no-op provider accepts every event without throwing") {
	auto p = tools::MakeNoOpAnalyticsProvider();
	REQUIRE(p != nullptr);
	CHECK_NOTHROW(p->OnSessionStart());
	CHECK_NOTHROW(p->OnToolCall({"read_blueprint",
								  std::chrono::milliseconds{12},
								  /*isError=*/false}));
	CHECK_NOTHROW(p->OnToolCall({"transpile_blueprint",
								  std::chrono::milliseconds{350},
								  /*isError=*/true}));
	CHECK_NOTHROW(p->OnSessionEnd());
}

TEST_CASE("Analytics: no-op provider survives many tool-call events") {
	auto p = tools::MakeNoOpAnalyticsProvider();
	p->OnSessionStart();
	for (int i = 0; i < 1000; ++i) {
		p->OnToolCall({"list_blueprints",
					   std::chrono::milliseconds{i % 50},
					   /*isError=*/(i % 7 == 0)});
	}
	p->OnSessionEnd();
	// If the no-op provider had a bug (e.g. unbounded allocation),
	// we'd see it here. Smoke check.
	CHECK(true);
}

// =====================================================================
// AnalyticsEnabled env gate
// =====================================================================

TEST_CASE("AnalyticsEnabled: returns false when env var unset (default)") {
	// We can't reliably unset env vars portably in a test, so assume
	// the test env doesn't set BP_READER_ANALYTICS=1. This matches
	// the default for both dev + CI runs.
	const char* current = std::getenv("BP_READER_ANALYTICS");
	if (current == nullptr || *current == '\0') {
		CHECK_FALSE(tools::AnalyticsEnabled());
	} else {
		// If the test env DOES set it, just check that the func didn't
		// crash. We can't assert false in that case.
		(void)tools::AnalyticsEnabled();
	}
}

// =====================================================================
// EULA notice
// =====================================================================

TEST_CASE("EulaNotice: non-empty and mentions privacy + the key env vars") {
	const std::string n = tools::EulaNotice();
	CHECK_FALSE(n.empty());
	CHECK(n.find("privacy") != std::string::npos);
	CHECK(n.find("BP_READER_ANALYTICS") != std::string::npos);
	CHECK(n.find("BP_READER_LOG_LEVEL") != std::string::npos);
	// The notice should make clear that the default is "no third
	// parties" — that's the most important user-facing fact.
	CHECK(n.find("third parties") != std::string::npos);
}

TEST_CASE("EulaNotice: deterministic between calls (same text every time)") {
	const auto a = tools::EulaNotice();
	const auto b = tools::EulaNotice();
	CHECK(a == b);
}

// =====================================================================
// ToolCallSample shape
// =====================================================================

TEST_CASE("ToolCallSample: fields are populated as expected") {
	const tools::ToolCallSample s{
		"read_blueprint",
		std::chrono::milliseconds{42},
		/*isError=*/false,
	};
	CHECK(s.tool == "read_blueprint");
	CHECK(s.elapsed.count() == 42);
	CHECK_FALSE(s.isError);
}

TEST_CASE("ToolCallSample: error sample carries isError=true") {
	const tools::ToolCallSample s{
		"add_variable",
		std::chrono::milliseconds{15},
		/*isError=*/true,
	};
	CHECK(s.isError);
}
