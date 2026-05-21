// MCP logging primitive — RFC 5424 severity levels emitted as
// `notifications/message` to subscribed clients. Pairs with the
// `logging/setLevel` request handler which adjusts the server's
// emit filter.
//
// MCP semantics (2025-06-18 spec):
//   - Levels are the RFC 5424 set: debug, info, notice, warning,
//     error, critical, alert, emergency.
//   - notifications/message carries { level, logger?, data }.
//   - logging/setLevel takes { level } and adjusts what severities
//     reach the wire. Default: info.
//
// Implementation notes:
//   - The Logger holds a raw pointer to jsonrpc::Server (server
//     outlives the logger in main.cpp's setup).
//   - Thread-safe via std::atomic<LogLevel> for the level field.
//     QueueNotification on the Server is itself thread-safe.
//   - No allocation in the hot path when the level filter rejects.
//   - stderr LogBlueprintReaderMcp continues to fire alongside —
//     the MCP notification is additive, not a replacement. Clients
//     that didn't subscribe still see the stderr trail.
#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bpr::jsonrpc {
class Server;
}

namespace bpr::tools {

// RFC 5424 severities, ordered low-to-high. Comparing two levels with
// <= "is no more severe than" works directly off the underlying int.
enum class LogLevel : int {
	Debug     = 0,
	Info      = 1,
	Notice    = 2,
	Warning   = 3,
	Error     = 4,
	Critical  = 5,
	Alert     = 6,
	Emergency = 7,
};

// Wire-format names per RFC 5424 / MCP spec.
std::string_view LogLevelName(LogLevel l);

// Parse a wire-format name back to a level. Case-insensitive.
// Returns std::nullopt for unrecognised strings.
//
// Special value "off" returns Emergency+1 (i.e. all levels filtered).
// Handled in SetLevelFromString — exposed here for tests.
bool TryParseLogLevel(std::string_view name, LogLevel& out);

class Logger {
public:
	// `server` must outlive this Logger. Notifications go through
	// server->QueueNotification.
	explicit Logger(jsonrpc::Server* server);

	// Set the minimum severity that reaches the wire. Logs below
	// this level are silently filtered. Default: Info.
	void SetLevel(LogLevel level);
	LogLevel GetLevel() const;

	// Set the level from a wire-format name. Returns false on
	// unrecognised input. Accepts the 8 RFC 5424 names plus "off"
	// (which sets the filter so nothing passes).
	bool SetLevelFromString(std::string_view name);

	// Emit a log notification. When `level` is below the current
	// filter, returns immediately without touching the Server.
	// `data` is the spec-defined data field; pass any JSON value.
	// `logger_name` is optional — typically the subsystem doing
	// the logging ("backend", "transport", "tools/apply_ops", ...).
	void Log(LogLevel level, nlohmann::json data,
			 std::string_view logger_name = {});

	// Convenience: emit a text-only log line with no logger name.
	void Log(LogLevel level, std::string text);

	// Returns true when a Log() call at the given level would be
	// emitted (i.e. level >= current filter). Hot-path tools can
	// gate expensive log payload construction on this.
	bool WouldEmit(LogLevel level) const;

	// Disable all emission — the off-switch SetLevelFromString
	// honors. Equivalent to filtering everything including
	// Emergency. Distinct from "ConfigureNothing": this gives an
	// explicit user-toggleable disable.
	void Disable();
	bool IsDisabled() const;

private:
	jsonrpc::Server*     server_;
	std::atomic<int>     level_{static_cast<int>(LogLevel::Info)};
	std::atomic<bool>    disabled_{false};
};

}    // namespace bpr::tools
