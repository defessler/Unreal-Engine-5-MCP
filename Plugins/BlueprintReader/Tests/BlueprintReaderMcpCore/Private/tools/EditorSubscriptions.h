// Phase 10 (EA-push) — subscription registry for editor push events.
//
// Tracks which `notifications/editor/*` event types clients want, so the
// (future) editor-event path can skip Server::QueueNotification for
// unsubscribed types. Thread-safe: the HTTP transport touches this from
// connection threads.
//
// v1 is a GLOBAL registry (not yet per-session): the C5 SSE stream
// drains all queued notifications regardless of which connection
// subscribed. Per-session filtering + Last-Event-ID resumption are
// refinements. The kill-switch is structural — when BP_READER_PUSH_EVENTS
// is off (default), main.cpp passes nullptr and the editor/subscribe
// methods + the `editor` capability are never registered (so
// editor/subscribe yields -32601 Method not found, per the plan).

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bpr::tools {

class EditorSubscriptions {
public:
	// Register interest in a set of event types. An empty list means
	// "all events" (the default Tier-A subscription). Returns an opaque
	// subscription id the client passes to editor/unsubscribe.
	std::string Subscribe(const std::vector<std::string>& eventTypes);

	// Drop a subscription by id. Returns true if it existed.
	bool Unsubscribe(const std::string& id);

	// True when some active subscription covers this event type (or is an
	// all-events subscription). Gates emission on the editor-event path.
	bool IsSubscribed(const std::string& eventType) const;

	// Number of active subscriptions (diagnostics / tests).
	std::size_t Count() const;

private:
	mutable std::mutex mutex_;
	std::uint64_t nextId_ = 1;
	// id -> event-type set. An empty string in the set is the "all" sentinel.
	std::unordered_map<std::string, std::unordered_set<std::string>> subs_;
};

}    // namespace bpr::tools
