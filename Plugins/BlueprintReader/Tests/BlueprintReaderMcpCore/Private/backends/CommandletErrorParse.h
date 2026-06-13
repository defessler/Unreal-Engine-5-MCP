// Inline helper shared between CommandletBlueprintReader and its unit tests.
// Kept in a header (not the .cpp's anonymous namespace) so the output-file
// error parse can be exercised from doctest without a live UE editor.
//
// UX-P5 e1 (follow-up): when a commandlet op FAILS via EmitError it writes a
// structured `{"error":"<detail>"}` to its `-Out=` file AND returns a non-zero
// exit code — e.g. NodeRefError's "ambiguous prefix … known node GUIDs: …"
// did-you-mean text. The non-zero-exit path used to throw a generic
// "exit=4 / missing target" from the stderr tail and delete the file unread,
// losing that detail (and, through apply_ops' per-op {ok:false,error:…},
// collapsing it to a bare exit=4). ExtractStructuredError recovers it.
//
// Returns the `error` string iff the file EXISTS, parses to a JSON OBJECT, and
// has a NON-EMPTY string `error` field. Otherwise std::nullopt — so a bare
// `return 4` with no output file (a genuine asset-not-found) correctly falls
// through to the AssetNotFound classification the smoke + apply_ops rely on.
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
