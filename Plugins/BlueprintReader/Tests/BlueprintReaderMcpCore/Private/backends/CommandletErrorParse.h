// Inline helper shared between CommandletBlueprintReader and its unit tests; in a
// header (not the .cpp's anonymous namespace) so the parse is exercisable from
// doctest without a live editor. A failed commandlet op writes {"error":"<detail>"}
// to its -Out= file AND returns non-zero; the exit-code path would otherwise throw
// a generic "exit=N" from the stderr tail and delete the file unread, losing that
// detail. ExtractStructuredError returns the `error` string when the file is a JSON
// object with a non-empty `error` field, else nullopt — so a bare non-zero exit with
// no output file still falls through to the AssetNotFound classification.
#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace bpr::backends::detail {

inline std::optional<std::string> ExtractStructuredError(
		const std::filesystem::path& outFile) {
	std::error_code ec;
	if (!std::filesystem::exists(outFile, ec)) {
		return std::nullopt;
	}
	std::ifstream in(outFile);
	if (!in.is_open()) {
		return std::nullopt;
	}
	// Tolerant parse — a non-JSON / truncated file yields a discarded value,
	// not an exception, so we just fall back to the generic exit-code path.
	const nlohmann::json j =
		nlohmann::json::parse(in, /*cb=*/nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) {
		return std::nullopt;
	}
	auto it = j.find("error");
	if (it == j.end() || !it->is_string()) {
		return std::nullopt;
	}
	std::string msg = it->get<std::string>();
	if (msg.empty()) {
		return std::nullopt;
	}
	return msg;
}

}    // namespace bpr::backends::detail
