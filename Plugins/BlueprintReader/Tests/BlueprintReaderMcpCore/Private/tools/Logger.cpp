#include "tools/Logger.h"

#include "jsonrpc/Server.h"

#include <algorithm>
#include <cctype>

namespace bpr::tools {

std::string_view LogLevelName(LogLevel l) {
	switch (l) {
		case LogLevel::Debug:     return "debug";
		case LogLevel::Info:      return "info";
		case LogLevel::Notice:    return "notice";
		case LogLevel::Warning:   return "warning";
		case LogLevel::Error:     return "error";
		case LogLevel::Critical:  return "critical";
		case LogLevel::Alert:     return "alert";
		case LogLevel::Emergency: return "emergency";
	}
	return "info";
}

bool TryParseLogLevel(std::string_view name, LogLevel& out) {
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower == "debug")     { out = LogLevel::Debug;     return true; }
	if (lower == "info")      { out = LogLevel::Info;      return true; }
	if (lower == "notice")    { out = LogLevel::Notice;    return true; }
	if (lower == "warning")   { out = LogLevel::Warning;   return true; }
	if (lower == "warn")      { out = LogLevel::Warning;   return true; }
	if (lower == "error")     { out = LogLevel::Error;     return true; }
	if (lower == "critical")  { out = LogLevel::Critical;  return true; }
	if (lower == "crit")      { out = LogLevel::Critical;  return true; }
	if (lower == "alert")     { out = LogLevel::Alert;     return true; }
	if (lower == "emergency") { out = LogLevel::Emergency; return true; }
	if (lower == "emerg")     { out = LogLevel::Emergency; return true; }
	return false;
}

Logger::Logger(jsonrpc::Server* server) : server_(server) {}

void Logger::SetLevel(LogLevel level) {
	level_.store(static_cast<int>(level), std::memory_order_release);
	// SetLevel re-enables emission — the explicit "off" sentinel only
	// triggers through SetLevelFromString("off") / Disable().
	disabled_.store(false, std::memory_order_release);
}

LogLevel Logger::GetLevel() const {
	return static_cast<LogLevel>(level_.load(std::memory_order_acquire));
}

bool Logger::SetLevelFromString(std::string_view name) {
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower == "off" || lower == "none" || lower == "0") {
		Disable();
		return true;
	}
	LogLevel parsed;
	if (!TryParseLogLevel(name, parsed)) {
		return false;
	}
	SetLevel(parsed);
	return true;
}

bool Logger::WouldEmit(LogLevel level) const {
	if (disabled_.load(std::memory_order_acquire)) {
		return false;
	}
	return static_cast<int>(level) >=
		level_.load(std::memory_order_acquire);
}

void Logger::Log(LogLevel level, nlohmann::json data,
				 std::string_view logger_name) {
	if (!WouldEmit(level)) {
		return;
	}
	if (server_ == nullptr) {
		return;
	}
	nlohmann::json params = {
		{"level", LogLevelName(level)},
		{"data",  std::move(data)},
	};
	if (!logger_name.empty()) {
		params["logger"] = std::string(logger_name);
	}
	server_->QueueNotification("notifications/message", std::move(params));
}

void Logger::Log(LogLevel level, std::string text) {
	Log(level, nlohmann::json(std::move(text)));
}

void Logger::Disable() {
	disabled_.store(true, std::memory_order_release);
}

bool Logger::IsDisabled() const {
	return disabled_.load(std::memory_order_acquire);
}

}    // namespace bpr::tools
