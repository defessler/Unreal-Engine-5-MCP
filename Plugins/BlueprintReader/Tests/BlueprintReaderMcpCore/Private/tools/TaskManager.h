// MCP-8: async Tasks primitive (MCP 2025-11-25, experimental).
//
// A long-running tool call (cook_content, package_project, build_lighting,
// run_automation_tests, a big apply_ops) can be augmented with a `task`
// object. Instead of blocking the client until the op finishes — which trips
// some clients' own request timeouts — the server returns a taskId
// immediately and runs the op on a BACKGROUND THREAD. The client then polls
// `tasks/get` (and can `tasks/cancel`) without holding the connection open on
// a single in-flight request.
//
// SINGLE-TASK MODEL. The backend (one socket to the editor / one commandlet
// subprocess) is a shared, non-reentrant resource — two ops running at once
// would corrupt its wire state. So at most one task runs at a time, and while
// it runs the server rejects other tool calls with a clear "busy" error (but
// tasks/get + tasks/cancel + tasks/list stay responsive, since they never
// touch the backend). This keeps the stdio read loop responsive: starting a
// task returns instantly, and the poll/cancel methods are registry-only.
//
// Cancellation reuses the existing CallContext + Server in-flight registry:
// the WorkFn (built in Mcp.cpp) registers its CallContext under the taskId, so
// tasks/cancel → Server::FindInFlight(taskId)->MarkCancelled() flips the same
// cooperative flag a synchronous call would see.
//
// NOTE: the 2025-11-25 tasks spec is experimental and is redesigned in the
// 2026-07-28 GA; the wire shape here tracks 2025-11-25 and may need a small
// update at GA. The infrastructure (registry, background execution, cancel)
// is spec-version-independent.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools {

class TaskManager {
public:
	// Runs on a background thread. Receives the assigned taskId (so it can set
	// up a CallContext registered under that id for cancellation). Returns the
	// finished CallToolResult envelope (success or isError). Must not throw for
	// normal tool errors — only an unexpected exception marks the task failed.
	using WorkFn = std::function<nlohmann::json(const std::string& taskId)>;

	struct View {
		std::string taskId;
		std::string tool;
		std::string status;    // working | completed | failed | cancelled
		bool hasResult = false;
		nlohmann::json result;    // the CallToolResult envelope when finished
		std::int64_t ttlMs = 0;
	};

	TaskManager() : state_(std::make_shared<State>()) {}

	~TaskManager() {
		// Detach any live workers. Each worker holds its own State + Task via
		// shared_ptr, so it is safe to keep running after `this` is gone; the
		// objects survive until the thread releases them.
		std::lock_guard<std::mutex> lk(state_->mu);
		for (auto& [id, t] : state_->tasks) {
			if (t->worker.joinable()) {
				t->worker.detach();
			}
		}
	}

	TaskManager(const TaskManager&) = delete;
	TaskManager& operator=(const TaskManager&) = delete;

	// Start a task. Returns the new taskId, or nullopt if a task is already
	// active (the shared backend is exclusive — the caller should surface a
	// "busy" error).
	std::optional<std::string> Start(const std::string& tool,
									 std::int64_t ttlMs, WorkFn work) {
		auto st = state_;
		std::lock_guard<std::mutex> lk(st->mu);
		if (!st->activeId.empty()) {
			return std::nullopt;
		}
		const std::string id = "task-" + std::to_string(++st->counter);
		auto t = std::make_shared<Task>();
		t->tool = tool;
		t->status = "working";
		t->ttlMs = ttlMs;
		st->tasks[id] = t;
		st->activeId = id;
		// Worker captures only shared_ptrs (st, t) — never `this` — so it is
		// safe to outlive the TaskManager (detached in the dtor).
		t->worker = std::thread([st, t, id, work]() {
			nlohmann::json env;
			std::string status;
			try {
				env = work(id);
				status = "completed";
			} catch (const std::exception& e) {
				env = {
					{"content", nlohmann::json::array({
						nlohmann::json{{"type", "text"},
									   {"text", std::string("task failed: ") + e.what()}}})},
					{"isError", true},
				};
				status = "failed";
			}
			std::lock_guard<std::mutex> wl(st->mu);
			t->result = std::move(env);
			t->hasResult = true;
			t->status = t->cancelRequested ? "cancelled" : status;
			st->activeId.clear();    // free the slot for the next task
		});
		return id;
	}

	bool HasActive() const {
		std::lock_guard<std::mutex> lk(state_->mu);
		return !state_->activeId.empty();
	}

	std::optional<View> Get(const std::string& taskId) const {
		std::lock_guard<std::mutex> lk(state_->mu);
		auto it = state_->tasks.find(taskId);
		if (it == state_->tasks.end()) {
			return std::nullopt;
		}
		return ToView(taskId, *it->second);
	}

	std::vector<View> List() const {
		std::lock_guard<std::mutex> lk(state_->mu);
		std::vector<View> out;
		out.reserve(state_->tasks.size());
		for (const auto& [id, t] : state_->tasks) {
			out.push_back(ToView(id, *t));
		}
		return out;
	}

	// Flags the task cancel-requested (the actual cooperative cancel goes
	// through the Server in-flight registry in Mcp.cpp). Returns false if the
	// task is unknown. A finished task is a no-op (returns true, already done).
	bool MarkCancelRequested(const std::string& taskId) {
		std::lock_guard<std::mutex> lk(state_->mu);
		auto it = state_->tasks.find(taskId);
		if (it == state_->tasks.end()) {
			return false;
		}
		if (it->second->status == "working") {
			it->second->cancelRequested = true;
		}
		return true;
	}

private:
	struct Task {
		std::string tool;
		std::string status;
		nlohmann::json result;
		bool hasResult = false;
		bool cancelRequested = false;
		std::int64_t ttlMs = 0;
		std::thread worker;
	};
	struct State {
		std::mutex mu;
		std::map<std::string, std::shared_ptr<Task>> tasks;
		std::string activeId;
		std::uint64_t counter = 0;
	};

	static View ToView(const std::string& id, const Task& t) {
		View v;
		v.taskId = id;
		v.tool = t.tool;
		v.status = t.status;
		v.hasResult = t.hasResult;
		v.result = t.result;
		v.ttlMs = t.ttlMs;
		return v;
	}

	std::shared_ptr<State> state_;
};

}    // namespace bpr::tools
