// Per-tool-call ambient context: requestId, cancel flag, progress emit.
//
// Long-running tools (cook_content, package_project, run_automation_tests,
// build_lighting) need two things the synchronous tools/call path
// didn't provide:
//
//   * **Progress reporting** — emit `notifications/progress` so the
//     LLM (and the user watching its work) sees liveness, not a black
//     box for minutes.
//   * **Cancellation** — honor the client's `notifications/cancelled`
//     so an agent that changed its mind doesn't waste a 30-min cook.
//
// We thread these through as ambient (thread-local) state rather than
// adding parameters to ToolFn. Tools that don't care don't change.
// Tools that do care call CallContext::Current().EmitProgress() and
// poll CallContext::Current().IsCancelled() at convenient points in
// their work.
//
// MCP spec refs:
//   * https://modelcontextprotocol.io/specification/2025-06-18/server/utilities/progress
//   * https://modelcontextprotocol.io/specification/2025-06-18/server/utilities/cancellation
#pragma once

#include "jsonrpc/Server.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace bpr::jsonrpc {

// Ambient per-call state. One instance per in-flight tools/call. Set
// up by Mcp.cpp's dispatcher before invoking the tool fn, cleared on
// return.
//
// Cancellation is cooperative: tools that want to respond to client
// cancellation must poll IsCancelled() at safe points. Tools that
// don't poll keep running to completion — cancellation is a HINT, not
// a guarantee.
class CallContext {
public:
	// Construct a context for a request. RequestId may be null for
	// notifications (no-op calls — IsCancelled always false, EmitProgress
	// is a no-op since there's no client request to attach progress to).
	CallContext(Server& server,
				nlohmann::json requestId,
				std::optional<nlohmann::json> progressToken);

	// Mark this call cancelled. Set by the notifications/cancelled handler
	// when the client signals cancellation against this requestId. Safe
	// to call concurrently with IsCancelled().
	void MarkCancelled();

	// Cooperative cancellation poll. Tools call this at convenient
	// abort points; returns true once notifications/cancelled has been
	// processed for this requestId. Cheap (atomic load) so OK to call
	// in tight loops.
	bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }

	// Queue a notifications/progress for this call. No-op when:
	//   * The client didn't send a progressToken with the request
	//     (per spec, server MUST NOT send progress without one).
	//   * RequestId is null (a notification — nowhere to report to).
	//
	// `progress` and `total` are MCP-spec semantics: total is optional
	// (the unknown-duration case); message is human-readable status.
	void EmitProgress(double progress,
					  std::optional<double> total = std::nullopt,
					  std::string message = {});

	// Match-by-id helper used by notifications/cancelled.
	bool Matches(const nlohmann::json& requestId) const;

	const nlohmann::json& RequestId() const { return requestId_; }

	// ---- ambient access ---------------------------------------------------

	// Returns the active context for this thread, or nullptr when no
	// call is in progress. Tools call this to opt into progress / cancel.
	static CallContext* Current();

	// RAII scope guard — sets the thread-local context on construction,
	// clears on destruction. The dispatcher uses this to bracket each
	// tools/call invocation.
	class Scope {
	public:
		explicit Scope(CallContext* ctx);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	private:
		CallContext* prev_;
	};

private:
	Server& server_;
	nlohmann::json requestId_;
	std::optional<nlohmann::json> progressToken_;
	std::atomic<bool> cancelled_{false};
};

}  // namespace bpr::jsonrpc
