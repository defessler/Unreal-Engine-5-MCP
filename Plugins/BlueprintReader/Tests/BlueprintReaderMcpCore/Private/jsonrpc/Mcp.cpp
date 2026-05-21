#include "jsonrpc/Mcp.h"

#include "jsonrpc/CallContext.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>

namespace bpr::mcp {

namespace jr = bpr::jsonrpc;

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
					  const ServerInfo& info) {
	// -------- initialize ---------------------------------------------------
	server.Register("initialize", [info](const nlohmann::json& params) -> jr::Response {
		// Echo the client's protocolVersion if we recognize it; fall back to
		// our default. The MCP spec evolves and clients send a string like
		// "2024-11-05" or "2025-06-18". Echoing what they sent (when known)
		// is the spec-compliant negotiation behaviour — hardcoding our own
		// version forces older clients to either error or strip features.
		// We accept any version equal to or older than ours to be permissive.
		static const std::vector<std::string> kKnownVersions = {
			"2024-11-05", // initial public spec
			"2025-03-26", // tool annotations / progress
			"2025-06-18", // resources, etc.
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
		nlohmann::json result = {
			{"protocolVersion", negotiated},
			{"capabilities", {
				{"tools", {{"listChanged", true}}},
			}},
			{"serverInfo", {
				{"name", info.name},
				{"version", info.version},
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
		return jr::Response::Ok(nlohmann::json{ {"tools", registry.ListSpec()} });
	});

	// -------- tools/call ---------------------------------------------------
	server.Register("tools/call", [&registry, &server](const nlohmann::json& params) -> jr::Response {
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

		const auto t0 = std::chrono::steady_clock::now();
		auto elapsedMs = [&]() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
		};

		try {
			nlohmann::json toolResult = (*fn)(arguments);
			// Convention: tools return canonical JSON. We dump it as text
			// content so MCP clients (which expect a content array) can
			// surface it; the underlying JSON shape is what Claude consumes.
			nlohmann::json meta = {
				{"elapsed_ms", elapsedMs()},
				{"tool", name},
			};
			// Progressive disclosure: if the tool mutated the active
			// surface (e.g. `enable_tool_category`), queue a
			// tools/list_changed notification for the server's flush.
			// Run() picks it up after this response is written.
			if (registry.TakeListChangedFlag()) {
				server.QueueNotification("notifications/tools/list_changed",
										 nlohmann::json::object());
			}
			// Rich-content opt-in: a tool that wants to emit image / audio
			// / structuredContent / multi-block / audience-annotated
			// results returns `{"_mcp": {"content": [...],
			// "structuredContent": ...}}` via tools::content::Envelope().
			// Detect that sentinel and unpack directly into the spec-shaped
			// response without going through the dump-to-text path.
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
					return jr::Response::Ok(std::move(env));
				}
			}
			return jr::Response::Ok(MakeToolTextContent(toolResult.dump(2),
				/*isError=*/false, std::move(meta)));
		} catch (const std::exception& e) {
			// Enrich the error envelope with the call args so the agent
			// can see what triggered the failure without re-driving the
			// call. Helpful for "I called transpile_blueprint on
			// /Game/X and got 'asset not found'" diagnostics.
			nlohmann::json meta = {
				{"elapsed_ms", elapsedMs()},
				{"tool", name},
			};
			// Include the args verbatim — for agents debugging a tool
			// failure, the "what did I pass" context is exactly what
			// they need. No filtering: MCP tool args don't carry
			// credentials in this server.
			if (!arguments.empty())
			{
				meta["args"] = arguments;
			}
			return jr::Response::Ok(MakeToolTextContent(
				fmt::format("tool error: {}", e.what()), /*isError=*/true, std::move(meta)));
		}
	});
}

}    // namespace bpr::mcp
