#include "jsonrpc/Mcp.h"

#include <chrono>
#include <stdexcept>

#include <fmt/core.h>

namespace bpr::mcp {

namespace jr = bpr::jsonrpc;

namespace {

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

} // namespace

void RegisterHandlers(jr::Server& server,
                      const tools::ToolRegistry& registry,
                      const ServerInfo& info) {
    // -------- initialize ---------------------------------------------------
    server.Register("initialize", [info](const nlohmann::json& /*params*/) -> jr::Response {
        nlohmann::json result = {
            {"protocolVersion", info.protocolVersion},
            {"capabilities", {
                {"tools", nlohmann::json::object()}, // we serve tools, no list-changed notifs
            }},
            {"serverInfo", {
                {"name", info.name},
                {"version", info.version},
            }},
        };
        return jr::Response::Ok(std::move(result));
    });

    // -------- notifications/initialized -----------------------------------
    // Notification — return value is ignored by the dispatcher.
    server.Register("notifications/initialized",
        [](const nlohmann::json& /*params*/) -> jr::Response {
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
    server.Register("tools/call", [&registry](const nlohmann::json& params) -> jr::Response {
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
            return jr::Response::Ok(MakeToolTextContent(toolResult.dump(2),
                /*isError=*/false, std::move(meta)));
        } catch (const std::exception& e) {
            nlohmann::json meta = {
                {"elapsed_ms", elapsedMs()},
                {"tool", name},
            };
            return jr::Response::Ok(MakeToolTextContent(
                fmt::format("tool error: {}", e.what()), /*isError=*/true, std::move(meta)));
        }
    });
}

} // namespace bpr::mcp
