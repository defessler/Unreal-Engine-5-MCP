#include "jsonrpc/CallContext.h"

#include <utility>

namespace bpr::jsonrpc {

namespace {
// Thread-local ambient call context. One slot per dispatch thread.
// Server::Run dispatches single-threaded today, but the future HTTP
// transport will use multiple worker threads — using thread_local
// keeps each dispatch lane isolated.
thread_local CallContext* g_current_call_context = nullptr;
}  // namespace

CallContext::CallContext(Server& server,
						 nlohmann::json requestId,
						 std::optional<nlohmann::json> progressToken)
	: server_(server),
	  requestId_(std::move(requestId)),
	  progressToken_(std::move(progressToken)) {}

void CallContext::MarkCancelled() {
	cancelled_.store(true, std::memory_order_release);
}

void CallContext::EmitProgress(double progress, std::optional<double> total, std::string message) {
	if (!progressToken_.has_value()) {
		// Per spec: server MUST NOT send progress notifications without
		// a client-supplied progressToken. Silently no-op.
		return;
	}
	nlohmann::json params = {
		{"progressToken", *progressToken_},
		{"progress", progress},
	};
	if (total.has_value()) {
		params["total"] = *total;
	}
	if (!message.empty()) {
		params["message"] = std::move(message);
	}
	server_.QueueNotification("notifications/progress", std::move(params));
}

bool CallContext::Matches(const nlohmann::json& requestId) const {
	if (requestId_.is_null() && requestId.is_null()) {
		return true;
	}
	// nlohmann::json comparison handles number/string/null shapes correctly.
	return requestId_ == requestId;
}

CallContext* CallContext::Current() {
	return g_current_call_context;
}

CallContext::Scope::Scope(CallContext* ctx)
	: prev_(g_current_call_context) {
	g_current_call_context = ctx;
}

CallContext::Scope::~Scope() {
	g_current_call_context = prev_;
}

}  // namespace bpr::jsonrpc
