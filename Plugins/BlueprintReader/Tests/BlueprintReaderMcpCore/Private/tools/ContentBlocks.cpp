#include "tools/ContentBlocks.h"

#include <utility>

namespace bpr::tools::content {

namespace content_blocks_detail {

nlohmann::json AudienceArray(Audience aud) {
	switch (aud) {
		case Audience::User:      return nlohmann::json::array({"user"});
		case Audience::Assistant: return nlohmann::json::array({"assistant"});
		case Audience::Both:      return nlohmann::json::array({"user", "assistant"});
	}
	return nlohmann::json::array({"user", "assistant"});
}

void AttachAudience(nlohmann::json& block, Audience aud) {
	// Spec optimization: when targeting both (the implicit default per
	// the MCP spec), don't emit an annotations object — it's noise.
	// Older clients that don't understand `annotations` just see the
	// content block; new clients infer "both" from the absence.
	if (aud == Audience::Both) {
		return;
	}
	block["annotations"] = {{"audience", AudienceArray(aud)}};
}

// RFC 4648 standard base64 alphabet. nlohmann::json doesn't ship a
// base64 helper, and we don't already pull in OpenSSL or fmt's base64.
// Lightweight inline implementation — only used at result-build time,
// so encode throughput isn't critical.
std::string Base64Encode(const std::vector<uint8_t>& bytes) {
	static constexpr char kAlphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((bytes.size() + 2) / 3) * 4);
	size_t i = 0;
	while (i + 3 <= bytes.size()) {
		const uint32_t triple = (uint32_t(bytes[i]) << 16) |
								(uint32_t(bytes[i + 1]) << 8) |
								uint32_t(bytes[i + 2]);
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
		out.push_back(kAlphabet[triple & 0x3F]);
		i += 3;
	}
	if (i < bytes.size()) {
		const size_t rem = bytes.size() - i;
		uint32_t triple = uint32_t(bytes[i]) << 16;
		if (rem == 2) {
			triple |= uint32_t(bytes[i + 1]) << 8;
		}
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(rem == 2 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
		out.push_back('=');
	}
	return out;
}

}  // namespace content_blocks_detail
using namespace content_blocks_detail;

nlohmann::json Text(std::string text, Audience aud) {
	nlohmann::json block = {
		{"type", "text"},
		{"text", std::move(text)},
	};
	AttachAudience(block, aud);
	return block;
}

nlohmann::json ImageBase64(std::string base64, std::string mime_type, Audience aud) {
	nlohmann::json block = {
		{"type", "image"},
		{"data", std::move(base64)},
		{"mimeType", std::move(mime_type)},
	};
	AttachAudience(block, aud);
	return block;
}

nlohmann::json Image(const std::vector<uint8_t>& bytes, std::string mime_type, Audience aud) {
	return ImageBase64(Base64Encode(bytes), std::move(mime_type), aud);
}

nlohmann::json Envelope(std::vector<nlohmann::json> blocks, nlohmann::json structured) {
	nlohmann::json inner = {
		{"content", nlohmann::json::array()},
	};
	for (auto& b : blocks) {
		inner["content"].push_back(std::move(b));
	}
	if (!structured.is_null()) {
		inner["structuredContent"] = std::move(structured);
	}
	return nlohmann::json{{"_mcp", std::move(inner)}};
}

}  // namespace bpr::tools::content
