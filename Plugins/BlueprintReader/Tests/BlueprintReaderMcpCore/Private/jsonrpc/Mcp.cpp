#include "jsonrpc/Mcp.h"

#include "Env.h"
#include "jsonrpc/CallContext.h"
#include "tools/ToolAnnotations.h"  // IsDestructive (MCP-9 confirmation guard)
#include "tools/TaskManager.h"      // MCP-8 async tasks primitive

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>

namespace bpr::mcp {

namespace jr = bpr::jsonrpc;

void RegisterHandlersImpl(jr::Server& server,
						  tools::ToolRegistry& registry,
						  tools::prompts::PromptRegistry* prompts,
						  tools::Logger* logger,
						  tools::resources::ResourceRegistry* resources,
						  const ServerInfo& info,
						  tools::EditorSubscriptions* editorSubs = nullptr);

std::string DefaultInstructions() {
	// Onboarding text the LLM sees as system-prompt context at session
	// start. Keep tight — clients pay tokens for this. Reference the
	// `bp-reader` skill (deployed under `.claude/skills/bp-reader/`) for
	// detailed patterns instead of inlining them here.
	return
		"bp-reader-mcp exposes UE5 Blueprint introspection, mutation, "
		"BP<->C++ transpile, and editor-control tools. Conventions:\n"
		"\n"
		"- Asset paths use the package form: `/Game/AI/BP_Foo` (NOT "
		"`/Game/AI/BP_Foo.BP_Foo`).\n"
		"- BP<->C++ uses BPIR as the pivot: a versioned JSON AST. See "
		"`decompile_function`, `transpile_function`, `parse_cpp_function`, "
		"`write_generated_source`.\n"
		"- Backends: `mock` (no UE, fixtures only), `commandlet` (spawns "
		"editor headlessly), `live` (talks to running editor over TCP), "
		"`auto` (default — picks live if editor is open, else commandlet).\n"
		"- Multi-step writes against the same BP should go through "
		"`apply_ops` / `preview_ops` / `compile_function` for atomicity. "
		"See the `bp-batches` skill.\n"
		"- Write tools are blocked on the mock backend (read-only). "
		"Mutation requires a real editor.\n"
		"- The transpile family (`transpile_blueprint`, "
		"`write_generated_source`, etc.) is gated by env var "
		"`BP_READER_ALLOW_TRANSPILE=1`. Off by default.\n";
}

namespace mcp_detail {

nlohmann::json MakeToolTextContent(const std::string& text, bool isError,
								   nlohmann::json meta = nlohmann::json::object()) {
	nlohmann::json env = {
		{"content", nlohmann::json::array({
			nlohmann::json{
				{"type", "text"},
				{"text", text},
			}
		})},
		{"isError", isError},
	};
	if (!meta.empty()) {
		// MCP 2024-11-05 reserves `_meta` as the extension field on tool
		// result envelopes; clients that surface it see telemetry, others
		// ignore it.
		env["_meta"] = std::move(meta);
	}
	return env;
}

}    // namespace mcp_detail
using namespace mcp_detail;

void RegisterHandlers(jr::Server& server,
					  tools::ToolRegistry& registry,
					  tools::prompts::PromptRegistry& prompts,
					  const ServerInfo& info) {
	RegisterHandlersImpl(server, registry, &prompts, /*logger=*/nullptr,
						 /*resources=*/nullptr, info);
}

void RegisterHandlers(jr::Server& server,
					  tools::ToolRegistry& registry,
					  const ServerInfo& info) {
	RegisterHandlersImpl(server, registry, /*prompts=*/nullptr,
						 /*logger=*/nullptr, /*resources=*/nullptr, info);
}

void RegisterHandlers(jr::Server& server,
					  tools::ToolRegistry& registry,
					  tools::prompts::PromptRegistry* prompts,
					  tools::Logger* logger,
					  const ServerInfo& info) {
	RegisterHandlersImpl(server, registry, prompts, logger,
						 /*resources=*/nullptr, info);
}

void RegisterHandlers(jr::Server& server,
					  tools::ToolRegistry& registry,
					  tools::prompts::PromptRegistry* prompts,
					  tools::Logger* logger,
					  tools::resources::ResourceRegistry* resources,
					  const ServerInfo& info,
					  tools::EditorSubscriptions* editorSubs) {
	RegisterHandlersImpl(server, registry, prompts, logger, resources, info, editorSubs);
}

void RegisterHandlersImpl(jr::Server& server,
						  tools::ToolRegistry& registry,
						  tools::prompts::PromptRegistry* prompts,
						  tools::Logger* logger,
						  tools::resources::ResourceRegistry* resources,
						  const ServerInfo& info,
						  tools::EditorSubscriptions* editorSubs) {
	// MCP-8: shared async-task manager. Held by shared_ptr so the value-captured
	// copy in each handler lambda keeps it alive for the server's lifetime.
	auto tasks = std::make_shared<tools::TaskManager>();
	// Tools that legitimately run for minutes — advertised with
	// execution.taskSupport and the prime users of the `task` augmentation.
	// (The augmentation is honored for ANY tool; only these advertise support
	// so clients know which ones benefit.)
	static const std::set<std::string, std::less<>> kLongRunningTools = {
		"build_lighting", "cook_content", "package_project",
		"run_automation_tests", "compile_blueprint", "apply_ops",
		"start_profile", "live_coding_compile",
	};

	// -------- initialize ---------------------------------------------------
	server.Register("initialize", [info, prompts, logger, resources, editorSubs](const nlohmann::json& params) -> jr::Response {
		// Echo the client's protocolVersion if we recognize it; fall back to
		// our default. The MCP spec evolves and clients send a string like
		// "2024-11-05" or "2025-11-25". Echoing what they sent (when known)
		// is the spec-compliant negotiation behaviour — hardcoding our own
		// version forces older clients to either error or strip features.
		// We accept any version equal to or older than ours to be permissive.
		static const std::vector<std::string> kKnownVersions = {
			"2024-11-05", // initial public spec
			"2025-03-26", // tool annotations / progress
			"2025-06-18", // resources, etc.
			"2025-11-25", // tasks primitive (MCP-8 — advertised + implemented)
		};
		std::string negotiated = info.protocolVersion;
		if (params.is_object()) {
			auto pv = params.find("protocolVersion");
			if (pv != params.end() && pv->is_string()) {
				std::string requested = pv->get<std::string>();
				for (const auto& v : kKnownVersions) {
					if (v == requested) { negotiated = requested; break; }
				}
			}
		}

		// We advertise the `listChanged` capability so progressive-
		// disclosure clients know to re-fetch tools/list when they
		// see `notifications/tools/list_changed`. Doing this
		// unconditionally is fine: in non-progressive sessions the
		// notification simply never fires, so well-behaved clients
		// don't pay any cost.
		nlohmann::json capabilities = {
			{"tools", {{"listChanged", true}}},
			// MCP-8: experimental async tasks (2025-11-25). We support the
			// `task` request augmentation on tools/call plus tasks/get,
			// tasks/cancel, and tasks/list.
			{"tasks", nlohmann::json::object()},
		};
		// Phase 3: advertise the prompts primitive when the host wired
		// a non-empty registry. Older clients ignore the field; new
		// ones know to fetch prompts/list. Suppress the capability
		// when no prompts are registered so we don't claim a primitive
		// that responds with an empty list (some clients are picky).
		if (prompts && prompts->Size() > 0) {
			capabilities["prompts"] = {{"listChanged", true}};
		}
		// Phase 6: logging primitive — advertised when a Logger is
		// wired in. The spec's logging capability object is currently
		// empty (no sub-flags), per
		// https://modelcontextprotocol.io/specification/2025-06-18/server/logging.
		if (logger != nullptr) {
			capabilities["logging"] = nlohmann::json::object();
		}
		// Phase 4: resources primitive. Spec value is currently an empty
		// object (the optional `subscribe`/`listChanged` flags default
		// to false). Suppress when no providers are wired.
		if (resources != nullptr && resources->ProviderCount() > 0) {
			capabilities["resources"] = nlohmann::json::object();
		}
		// Phase 10 (EA-push): advertise the editor push-events capability
		// under `experimental` (non-standard cap) when subscriptions are
		// wired. Clients opt in via editor/subscribe. Off by default
		// (editorSubs nullptr → not advertised, methods not registered).
		if (editorSubs != nullptr) {
			capabilities["experimental"]["editor"] = {{"events", true}};
		}
		nlohmann::json result = {
			{"protocolVersion", negotiated},
			{"capabilities", capabilities},
			{"serverInfo", {
				{"name",        info.name},
				{"version",     info.version},
				{"description", info.description},
			}},
		};
		// MCP `instructions` is optional. Ship only when ServerInfo has
		// a non-empty value (main.cpp sets the default; env var can clear).
		if (!info.instructions.empty()) {
			result["instructions"] = info.instructions;
		}
		return jr::Response::Ok(std::move(result));
	});

	// -------- notifications/initialized -----------------------------------
	// Notification — return value is ignored by the dispatcher.
	server.Register("notifications/initialized",
		[](const nlohmann::json& /*params*/) -> jr::Response {
			return jr::Response::Ok(nlohmann::json::object());
		});

	// -------- notifications/cancelled -------------------------------------
	// Per MCP 2025-06-18 §utilities/cancellation. Client signals that
	// a previously-issued requestId should be aborted. Look up the
	// in-flight CallContext for that id and mark it cancelled; the
	// tool polls IsCancelled() at safe points.
	//
	// In the current single-threaded stdio model the in-flight
	// registry is always empty when this handler runs (a tool call
	// either returned or threw before we got to read the next frame),
	// so the lookup is a no-op — same observable behaviour as before.
	// The wiring is now in place so the future async/HTTP path honors
	// cancellation without further refactoring.
	server.Register("notifications/cancelled",
		[&server](const nlohmann::json& params) -> jr::Response {
			if (params.is_object()) {
				if (auto idIt = params.find("requestId"); idIt != params.end()) {
					if (auto* ctx = server.FindInFlight(*idIt)) {
						ctx->MarkCancelled();
					}
				}
			}
			return jr::Response::Ok(nlohmann::json::object());
		});

	// -------- ping ---------------------------------------------------------
	server.Register("ping", [](const nlohmann::json& /*params*/) -> jr::Response {
		return jr::Response::Ok(nlohmann::json::object());
	});

	// -------- tools/list ---------------------------------------------------
	server.Register("tools/list", [&registry](const nlohmann::json& /*params*/) -> jr::Response {
		auto spec = registry.ListSpec();
		// MCP-8: advertise execution.taskSupport on the long-running tools so
		// clients know which calls accept the `task` augmentation (kLongRunning
		// Tools is a static local — visible here without capture).
		for (auto& t : spec) {
			if (t.is_object()) {
				auto n = t.find("name");
				if (n != t.end() && n->is_string() &&
					kLongRunningTools.count(n->get<std::string>()) != 0) {
					t["execution"] = {{"taskSupport", "optional"}};
				}
			}
		}
		return jr::Response::Ok(nlohmann::json{ {"tools", std::move(spec)} });
	});

	// -------- tools/call ---------------------------------------------------
	server.Register("tools/call", [&registry, &server, tasks](const nlohmann::json& params) -> jr::Response {
		if (!params.is_object()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				"tools/call params must be an object");
		}
		auto nameIt = params.find("name");
		if (nameIt == params.end() || !nameIt->is_string()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				R"(tools/call missing string "name")");
		}
		std::string name = nameIt->get<std::string>();

		nlohmann::json arguments = nlohmann::json::object();
		auto argIt = params.find("arguments");
		if (argIt != params.end()) {
			if (!argIt->is_object()) {
				return jr::Response::Fail(jr::ErrorCode::InvalidParams,
					R"(tools/call "arguments" must be an object)");
			}
			arguments = *argIt;
		}

		// MCP-8: detect the `task` augmentation (carried in tools/call params).
		// When present, the call runs on a background thread and we return a
		// taskId immediately so the client can poll tasks/get instead of
		// blocking on this request. ttl (ms) is how long the finished result
		// is retained; default 60s.
		bool taskMode = false;
		std::int64_t taskTtlMs = 60000;
		if (auto taskIt = params.find("task"); taskIt != params.end() && taskIt->is_object()) {
			taskMode = true;
			if (auto ttlIt = taskIt->find("ttl"); ttlIt != taskIt->end() && ttlIt->is_number()) {
				taskTtlMs = ttlIt->get<std::int64_t>();
			}
		}
		// MCP-8 (registry-race fix): tools that MUTATE the lock-free ToolRegistry
		// must not run on a background thread, because the stdio read loop keeps
		// serving tools/list (ListSpec) concurrently. enable_tool_category
		// rewrites the active set; call_tool can dispatch it. Both are instant
		// meta-tools — run them synchronously even if a `task` was requested.
		if (taskMode && (name == "enable_tool_category" || name == "call_tool")) {
			taskMode = false;
		}
		// MCP-8: single-task model — the editor backend (one socket / one
		// commandlet subprocess) is exclusive, so while a task runs we reject
		// any tools/call (sync OR a second task) with a clear busy error. The
		// read loop stays responsive because tasks/get + tasks/cancel never
		// touch the backend.
		if (tasks->HasActive()) {
			return jr::Response::Ok(MakeToolTextContent(
				"a background task is already running — the editor backend is "
				"exclusive. Poll tasks/get or tasks/cancel it before issuing "
				"another tool call.",
				/*isError=*/true));
		}

		// Extract optional progressToken from _meta per MCP 2025-06-18.
		// Tools that want to emit progress notifications use
		// CallContext::Current()->EmitProgress(...). Long-running tools
		// (cook_content, package_project, run_automation_tests,
		// build_lighting) are the prime users; others ignore.
		std::optional<nlohmann::json> progressToken;
		if (auto metaIt = params.find("_meta"); metaIt != params.end() && metaIt->is_object()) {
			if (auto ptIt = metaIt->find("progressToken"); ptIt != metaIt->end()) {
				progressToken = *ptIt;
			}
		}
		// The request id we're handling — set by Server when it called
		// us. We don't have direct access here (Server::Dispatch passes
		// only params to the handler), so we pull it from… we don't have
		// it. Note: id is the JSON-RPC envelope id, not visible at this
		// layer. We use a null requestId for the call context; tools that
		// want progress get it via progressToken alone (which is the spec-
		// correct identifier for progress). Cancellation matching uses
		// the same token — most clients echo the request id.
		jr::CallContext callCtx(server,
								progressToken.value_or(nlohmann::json()),
								progressToken);
		jr::CallContext::Scope scope(&callCtx);
		// Register in the server's in-flight registry so
		// notifications/cancelled can find this context by requestId.
		// Today's single-threaded dispatch means we're already past the
		// cancellation window by the time the registry would be consulted,
		// but the future async/HTTP path needs this hook.
		server.RegisterInFlight(&callCtx);
		struct UnregisterOnExit {
			jr::Server& s; jr::CallContext* c;
			~UnregisterOnExit() { s.UnregisterInFlight(c); }
		} unreg{server, &callCtx};

		const tools::ToolFn* fn = registry.Find(name);
		if (fn == nullptr) {
			// MCP convention: unknown tool — return as MCP tool error
			// envelope, not a JSON-RPC method-not-found.
			return jr::Response::Ok(MakeToolTextContent(
				fmt::format("unknown tool: {}", name), /*isError=*/true));
		}
		// MCP-9: destructive-op guard. When BP_READER_REQUIRE_CONFIRM=1,
		// any tool in the DestructiveSet must be called with _confirm:true
		// in arguments to proceed. This guards against accidental irreversible
		// mutations — the agent gets a clear "add _confirm:true to proceed"
		// error rather than silently deleting data.
		{
			static const bool kRequireConfirm = [](){
				auto v = bpr::env::Get("BP_READER_REQUIRE_CONFIRM");
				return v && *v != "0";
			}();
			if (kRequireConfirm && tools::IsDestructive(name)) {
				bool confirmed = arguments.is_object() &&
					arguments.value("_confirm", false);
				if (!confirmed) {
					return jr::Response::Ok(MakeToolTextContent(
						fmt::format("tool '{}' is destructive and requires "
							"explicit confirmation. Pass _confirm:true in "
							"arguments to proceed, or unset "
							"BP_READER_REQUIRE_CONFIRM to disable this guard.",
							name),
						/*isError=*/true));
				}
			}
		}

		// MCP-8: run-and-wrap — execute the tool fn (under whatever CallContext
		// is ambient on the running thread) and produce the spec-shaped
		// CallToolResult envelope. Shared by the synchronous path and the
		// background-task worker so both yield an identical result shape.
		auto executeAndWrap =
			[&registry, &server, fn, name](const nlohmann::json& callArgs) -> nlohmann::json {
			const auto t0 = std::chrono::steady_clock::now();
			auto elapsedMs = [&]() {
				return std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
			};
			try {
				nlohmann::json toolResult = (*fn)(callArgs);
				nlohmann::json meta = {
					{"elapsed_ms", elapsedMs()},
					{"tool", name},
				};
				// Progressive disclosure: a tool that mutated the active surface
				// (e.g. enable_tool_category) queues a tools/list_changed notif.
				if (registry.TakeListChangedFlag()) {
					server.QueueNotification("notifications/tools/list_changed",
											 nlohmann::json::object());
				}
				// Rich-content opt-in: `{"_mcp":{"content":[...],
				// "structuredContent":...}}` unpacks straight into the envelope.
				if (toolResult.is_object()) {
					if (auto mcpIt = toolResult.find("_mcp"); mcpIt != toolResult.end() && mcpIt->is_object()) {
						nlohmann::json env;
						if (auto contentIt = mcpIt->find("content"); contentIt != mcpIt->end()) {
							env["content"] = *contentIt;
						} else {
							env["content"] = nlohmann::json::array();
						}
						if (auto scIt = mcpIt->find("structuredContent"); scIt != mcpIt->end()) {
							env["structuredContent"] = *scIt;
						}
						env["isError"] = false;
						env["_meta"] = std::move(meta);
						return env;
					}
				}
				// UX-P4e: object results carry the full payload exactly once as
				// structuredContent; content[0].text is just a pointer note.
				if (toolResult.is_object()) {
					nlohmann::json env = MakeToolTextContent(
						"structured result returned (see structuredContent)",
						/*isError=*/false, std::move(meta));
					env["structuredContent"] = std::move(toolResult);
					return env;
				}
				return MakeToolTextContent(toolResult.dump(2),
					/*isError=*/false, std::move(meta));
			} catch (const std::exception& e) {
				nlohmann::json meta = {
					{"elapsed_ms", elapsedMs()},
					{"tool", name},
				};
				if (!callArgs.empty()) {
					meta["args"] = callArgs;
				}
				return MakeToolTextContent(
					fmt::format("tool error: {}", e.what()), /*isError=*/true, std::move(meta));
			}
		};

		// MCP-8 task path: spawn the op on a background thread + return a taskId
		// immediately. The worker sets up its OWN CallContext keyed by the
		// taskId, so tasks/cancel (→ Server::FindInFlight(taskId)) flips the
		// same cooperative cancel flag the tool polls. progressToken rides along
		// so progress notifications still flow (drained on the next write).
		if (taskMode) {
			auto startedId = tasks->Start(name, taskTtlMs,
				[&server, executeAndWrap, arguments, progressToken](const std::string& taskId,
						const std::function<void()>& markReady) -> nlohmann::json {
					jr::CallContext taskCtx(server, nlohmann::json(taskId), progressToken);
					jr::CallContext::Scope taskScope(&taskCtx);
					server.RegisterInFlight(&taskCtx);
					// The context is now findable by taskId — release Start so the
					// returned taskId is immediately cancellable (no registration
					// TOCTOU vs. a fast tasks/cancel).
					markReady();
					struct UnregTask {
						jr::Server& s; jr::CallContext* c;
						~UnregTask() { s.UnregisterInFlight(c); }
					} unregTask{server, &taskCtx};
					return executeAndWrap(arguments);
				});
			if (!startedId) {
				return jr::Response::Ok(MakeToolTextContent(
					"a background task is already running.", /*isError=*/true));
			}
			return jr::Response::Ok(nlohmann::json{
				{"task", {
					{"taskId", *startedId},
					{"status", "working"},
					{"ttl",    taskTtlMs},
				}},
			});
		}

		// Synchronous path (default) — runs under the main-thread callCtx above.
		return jr::Response::Ok(executeAndWrap(arguments));
	});

	// -------- tasks/get + tasks/cancel + tasks/list (MCP-8) ----------------
	// Poll / cancel / enumerate background tasks started via the `task`
	// augmentation on tools/call. These are registry-only (never touch the
	// editor backend) so they stay responsive while a task is running.
	server.Register("tasks/get", [tasks](const nlohmann::json& params) -> jr::Response {
		if (!params.is_object()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				"tasks/get params must be an object");
		}
		auto idIt = params.find("taskId");
		if (idIt == params.end() || !idIt->is_string()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				R"(tasks/get missing string "taskId")");
		}
		auto v = tasks->Get(idIt->get<std::string>());
		if (!v) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				fmt::format("unknown taskId: {}", idIt->get<std::string>()));
		}
		nlohmann::json result = {
			{"task", {
				{"taskId", v->taskId},
				{"status", v->status},
				{"ttl",    v->ttlMs},
			}},
		};
		// When the task has finished, return its CallToolResult under `result`
		// (the same envelope a synchronous tools/call would have produced).
		if (v->hasResult) {
			result["result"] = v->result;
		}
		return jr::Response::Ok(std::move(result));
	});

	server.Register("tasks/cancel", [tasks, &server](const nlohmann::json& params) -> jr::Response {
		if (!params.is_object()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				"tasks/cancel params must be an object");
		}
		auto idIt = params.find("taskId");
		if (idIt == params.end() || !idIt->is_string()) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				R"(tasks/cancel missing string "taskId")");
		}
		const std::string id = idIt->get<std::string>();
		if (!tasks->MarkCancelRequested(id)) {
			return jr::Response::Fail(jr::ErrorCode::InvalidParams,
				fmt::format("unknown taskId: {}", id));
		}
		// Flip the cooperative cancel flag on the running task's CallContext
		// (registered under the taskId by the worker). Cancellation is a HINT:
		// the tool must poll CallContext::IsCancelled() at a safe point.
		if (auto* ctx = server.FindInFlight(nlohmann::json(id))) {
			ctx->MarkCancelled();
		}
		auto v = tasks->Get(id);
		return jr::Response::Ok(nlohmann::json{
			{"task", {
				{"taskId", id},
				{"status", v ? v->status : std::string("cancelled")},
			}},
		});
	});

	server.Register("tasks/list", [tasks](const nlohmann::json& /*params*/) -> jr::Response {
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& v : tasks->List()) {
			arr.push_back(nlohmann::json{
				{"taskId", v.taskId},
				{"tool",   v.tool},
				{"status", v.status},
				{"ttl",    v.ttlMs},
			});
		}
		return jr::Response::Ok(nlohmann::json{{"tasks", std::move(arr)}});
	});

	// -------- logging/setLevel --------------------------------------------
	// Phase 6 — MCP 2025-06-18 logging primitive. Only register when a
	// Logger was provided. Adjusts which severity reaches the wire as
	// `notifications/message`. Default level (set in Logger ctor) is
	// info; clients downgrade to debug for triage or upgrade to error
	// for noise reduction.
	if (logger != nullptr) {
		server.Register("logging/setLevel",
			[logger](const nlohmann::json& params) -> jr::Response {
				if (!params.is_object()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						"logging/setLevel params must be an object");
				}
				auto levelIt = params.find("level");
				if (levelIt == params.end() || !levelIt->is_string()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						R"(logging/setLevel missing string "level")");
				}
				const std::string level = levelIt->get<std::string>();
				if (!logger->SetLevelFromString(level)) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						fmt::format(
							"unknown log level '{}' — expected one of "
							"debug, info, notice, warning, error, "
							"critical, alert, emergency, off",
							level));
				}
				return jr::Response::Ok(nlohmann::json::object());
			});
	}

	// -------- editor/subscribe + editor/unsubscribe -----------------------
	// Phase 10 (EA-push) — subscription model for editor push events.
	// Registered only when push events are enabled (editorSubs non-null);
	// otherwise these methods are absent and the server replies -32601,
	// honoring the BP_READER_PUSH_EVENTS kill-switch. The Tier-A event
	// *sources* (editor UE-delegate -> Server::QueueNotification over the
	// live channel) are the follow-up; this is the client-facing model.
	if (editorSubs != nullptr) {
		server.Register("editor/subscribe",
			[editorSubs](const nlohmann::json& params) -> jr::Response {
				std::vector<std::string> types;
				if (params.is_object()) {
					auto it = params.find("event_types");
					if (it != params.end() && it->is_array()) {
						for (const auto& v : *it) {
							if (v.is_string()) {
								types.push_back(v.get<std::string>());
							}
						}
					}
				}
				return jr::Response::Ok(nlohmann::json{
					{"subscription_id", editorSubs->Subscribe(types)},
				});
			});

		server.Register("editor/unsubscribe",
			[editorSubs](const nlohmann::json& params) -> jr::Response {
				if (!params.is_object()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						"editor/unsubscribe params must be an object");
				}
				auto it = params.find("subscription_id");
				if (it == params.end() || !it->is_string()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						R"(editor/unsubscribe missing string "subscription_id")");
				}
				return jr::Response::Ok(nlohmann::json{
					{"ok", editorSubs->Unsubscribe(it->get<std::string>())},
				});
			});
	}

	// -------- resources/list + resources/read -----------------------------
	// Phase 4 — MCP 2025-06-18 resources primitive. Only register when
	// a ResourceRegistry was provided with at least one provider.
	if (resources != nullptr && resources->ProviderCount() > 0) {
		server.Register("resources/list",
			[resources](const nlohmann::json& /*params*/) -> jr::Response {
				return jr::Response::Ok(nlohmann::json{
					{"resources", resources->ListAll()},
				});
			});

		server.Register("resources/read",
			[resources](const nlohmann::json& params) -> jr::Response {
				if (!params.is_object()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						"resources/read params must be an object");
				}
				auto uriIt = params.find("uri");
				if (uriIt == params.end() || !uriIt->is_string()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						R"(resources/read missing string "uri")");
				}
				const std::string uri = uriIt->get<std::string>();
				if (!resources->Handles(uri)) {
					// MCP spec carves out -32002 ResourceNotFound for
					// "no provider serves this URI". Use it instead of
					// the generic InvalidParams.
					return jr::Response::Fail(/*code=*/-32002,
						fmt::format(
							"no resource provider handles URI '{}'; "
							"call resources/list to see what's available", uri));
				}
				try {
					return jr::Response::Ok(resources->Read(uri));
				} catch (const std::exception& e) {
					return jr::Response::Fail(/*code=*/-32002,
						fmt::format("resources/read failed for '{}': {}",
									uri, e.what()));
				}
			});
	}

	// -------- prompts/list + prompts/get ----------------------------------
	// Phase 3 — slash-command UX. Only register when a non-null
	// PromptRegistry was provided AND it has at least one prompt;
	// otherwise these methods stay unregistered and clients see a
	// JSON-RPC method-not-found if they try (consistent with the
	// capability not being advertised on initialize).
	if (prompts && prompts->Size() > 0) {
		// `prompts` is captured by pointer (the registry outlives the
		// server in main.cpp's setup). PromptRegistry isn't const-safe
		// — Render is logically const but uses the descriptors_ map,
		// so we capture as a non-const pointer.
		server.Register("prompts/list",
			[prompts](const nlohmann::json& /*params*/) -> jr::Response {
				return jr::Response::Ok(nlohmann::json{
					{"prompts", prompts->ListSpec()},
				});
			});

		server.Register("prompts/get",
			[prompts](const nlohmann::json& params) -> jr::Response {
				if (!params.is_object()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						"prompts/get params must be an object");
				}
				auto nameIt = params.find("name");
				if (nameIt == params.end() || !nameIt->is_string()) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						R"(prompts/get missing string "name")");
				}
				const std::string name = nameIt->get<std::string>();
				nlohmann::json arguments = nlohmann::json::object();
				if (auto argIt = params.find("arguments"); argIt != params.end()) {
					if (argIt->is_object()) {
						arguments = *argIt;
					} else if (!argIt->is_null()) {
						return jr::Response::Fail(jr::ErrorCode::InvalidParams,
							R"(prompts/get "arguments" must be an object)");
					}
				}
				if (!prompts->Has(name)) {
					// MCP spec doesn't carve out a `prompt_not_found` error
					// code, so use the standard MethodNotFound — clients
					// recognize that as "the named primitive doesn't exist."
					return jr::Response::Fail(jr::ErrorCode::MethodNotFound,
						fmt::format("unknown prompt: {}", name));
				}
				try {
					return jr::Response::Ok(prompts->Render(name, arguments));
				} catch (const std::exception& e) {
					return jr::Response::Fail(jr::ErrorCode::InvalidParams,
						fmt::format("prompts/get failed: {}", e.what()));
				}
			});
	}
}

}    // namespace bpr::mcp
