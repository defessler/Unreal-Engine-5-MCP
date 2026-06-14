#include "tools/Cursor.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace bpr::tools {

namespace cursor_detail {

// Same RFC 4648 alphabet ContentBlocks::Base64Encode uses. We
// re-implement here to avoid a circular include between low-level
// tool helpers; the duplication is trivial.
constexpr const char kAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Encode(const std::string& bytes) {
	std::string out;
	out.reserve(((bytes.size() + 2) / 3) * 4);
	std::size_t i = 0;
	while (i + 3 <= bytes.size()) {
		const std::uint32_t triple =
			(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i    ])) << 16) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) <<  8) |
			 static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(kAlphabet[(triple >>  6) & 0x3F]);
		out.push_back(kAlphabet[ triple        & 0x3F]);
		i += 3;
	}
	if (i < bytes.size()) {
		const std::size_t rem = bytes.size() - i;
		std::uint32_t triple = static_cast<std::uint32_t>(
			static_cast<unsigned char>(bytes[i])) << 16;
		if (rem == 2) {
			triple |= static_cast<std::uint32_t>(
				static_cast<unsigned char>(bytes[i + 1])) << 8;
		}
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(rem == 2 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
		out.push_back('=');
	}
	return out;
}

// Decode lookup for the RFC 4648 alphabet. Returns 0xFF for non-alphabet
// chars so callers can reject malformed input cleanly.
std::array<std::uint8_t, 256> BuildDecodeTable() {
	std::array<std::uint8_t, 256> t{};
	for (auto& v : t) v = 0xFF;
	for (std::uint8_t i = 0; i < 64; ++i) {
		t[static_cast<unsigned char>(kAlphabet[i])] = i;
	}
	return t;
}

std::optional<std::string> Decode(std::string_view b64) {
	static const auto kTable = BuildDecodeTable();
	if (b64.empty()) {
		return std::nullopt;
	}
	if (b64.size() % 4 != 0) {
		return std::nullopt;
	}
	std::string out;
	out.reserve((b64.size() / 4) * 3);
	for (std::size_t i = 0; i < b64.size(); i += 4) {
		const auto c0 = b64[i];
		const auto c1 = b64[i + 1];
		const auto c2 = b64[i + 2];
		const auto c3 = b64[i + 3];
		const std::uint8_t v0 = kTable[static_cast<unsigned char>(c0)];
		const std::uint8_t v1 = kTable[static_cast<unsigned char>(c1)];
		if (v0 == 0xFF || v1 == 0xFF) {
			return std::nullopt;
		}
		out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
		if (c2 == '=') {
			if (c3 != '=' || i + 4 != b64.size()) {
				return std::nullopt;
			}
			break;
		}
		const std::uint8_t v2 = kTable[static_cast<unsigned char>(c2)];
		if (v2 == 0xFF) {
			return std::nullopt;
		}
		out.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
		if (c3 == '=') {
			if (i + 4 != b64.size()) {
				return std::nullopt;
			}
			break;
		}
		const std::uint8_t v3 = kTable[static_cast<unsigned char>(c3)];
		if (v3 == 0xFF) {
			return std::nullopt;
		}
		out.push_back(static_cast<char>(((v2 & 0x03) << 6) | v3));
	}
	return out;
}

}    // namespace cursor_detail
using namespace cursor_detail;

std::string EncodeCursor(std::int64_t offset) {
	if (offset < 0) {
		offset = 0;
	}
	nlohmann::json payload = {{"o", offset}};
	return Encode(payload.dump());
}

std::optional<std::int64_t> DecodeCursor(std::string_view cursor) {
	auto decoded = Decode(cursor);
	if (!decoded.has_value()) {
		return std::nullopt;
	}
	nlohmann::json parsed = nlohmann::json::parse(
		*decoded, /*cb=*/nullptr, /*allow_exceptions=*/false);
	if (parsed.is_discarded() || !parsed.is_object()) {
		return std::nullopt;
	}
	auto it = parsed.find("o");
	if (it == parsed.end() || !it->is_number_integer()) {
		return std::nullopt;
	}
	const std::int64_t offset = it->get<std::int64_t>();
	if (offset < 0) {
		return std::nullopt;
	}
	return offset;
}

}    // namespace bpr::tools
