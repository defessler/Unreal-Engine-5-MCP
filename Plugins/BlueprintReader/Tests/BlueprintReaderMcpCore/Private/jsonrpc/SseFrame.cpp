#include "jsonrpc/SseFrame.h"

#include <sstream>
#include <vector>

namespace bpr::jsonrpc::http {

// Split `s` on `\n` (treating `\r\n` as `\n`) and prefix each line
// with `data: `. Multi-line JSON payloads need this because the SSE
// spec terminates events on `\n\n`; a JSON object with embedded
// newlines would otherwise look like multiple events to the parser.
// nlohmann's compact `.dump()` doesn't produce \n, but a future
// pretty-printer path or escape-roundtrip might.
static std::string EmitDataLines(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 16);
	size_t i = 0;
	while (i < s.size()) {
		size_t nl = s.find('\n', i);
		size_t end = (nl == std::string::npos) ? s.size() : nl;
		out += "data: ";
		// Strip trailing \r if the source uses CRLF.
		size_t lineLen = end - i;
		if (lineLen > 0 && s[end - 1] == '\r') {
			--lineLen;
		}
		out.append(s, i, lineLen);
		out += '\n';
		if (nl == std::string::npos) break;
		i = nl + 1;
	}
	if (s.empty()) {
		out += "data: \n";
	}
	return out;
}

std::string FormatSseFrame(const std::string& event,
                            const nlohmann::json& body,
                            std::optional<std::string> id) {
	std::string out;
	out.reserve(128);
	if (!event.empty()) {
		out += "event: ";
		out += event;
		out += '\n';
	}
	if (id.has_value() && !id->empty()) {
		out += "id: ";
		out += *id;
		out += '\n';
	}
	out += EmitDataLines(body.dump());
	out += '\n';  // terminate frame
	return out;
}

std::string FormatNotificationFrame(const std::string& method,
                                     const nlohmann::json& params,
                                     std::optional<std::string> id) {
	nlohmann::json envelope = {
		{"jsonrpc", "2.0"},
		{"method",  method},
		{"params",  params},
	};
	return FormatSseFrame("message", envelope, std::move(id));
}

std::string FormatRetryFrame(int milliseconds) {
	std::string out = "retry: ";
	out += std::to_string(milliseconds);
	out += "\n\n";
	return out;
}

std::string FormatCommentFrame(const std::string& text) {
	std::string out;
	out.reserve(text.size() + 4);
	out += ':';
	if (!text.empty()) {
		out += ' ';
		out += text;
	}
	out += "\n\n";
	return out;
}

std::vector<SseEvent> ParseSseStream(const std::string& raw) {
	std::vector<SseEvent> events;
	SseEvent cur;
	bool hasContent = false;

	auto pushIfReady = [&]() {
		if (hasContent) {
			events.push_back(std::move(cur));
			cur = SseEvent{};
			hasContent = false;
		}
	};

	std::istringstream is(raw);
	std::string line;
	while (std::getline(is, line)) {
		// Strip trailing \r (CRLF tolerance).
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			pushIfReady();
			continue;
		}
		if (line[0] == ':') {
			// Comment line — ignored per spec.
			continue;
		}
		auto colon = line.find(':');
		std::string field = (colon == std::string::npos) ? line : line.substr(0, colon);
		std::string value;
		if (colon != std::string::npos) {
			value = line.substr(colon + 1);
			// Per spec, strip a single leading space from value.
			if (!value.empty() && value[0] == ' ') {
				value.erase(0, 1);
			}
		}
		if (field == "event") {
			cur.event = value;
			hasContent = true;
		} else if (field == "data") {
			if (!cur.data.empty()) {
				cur.data += '\n';
			}
			cur.data += value;
			hasContent = true;
		} else if (field == "id") {
			cur.id = value;
			hasContent = true;
		}
		// "retry" + unknown fields ignored.
	}
	pushIfReady();
	return events;
}

}    // namespace bpr::jsonrpc::http
