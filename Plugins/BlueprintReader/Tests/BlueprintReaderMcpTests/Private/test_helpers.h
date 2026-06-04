// Shared helpers for the doctest suite: locate the staged fixtures
// directory next to the test exe, and construct mock-backed readers
// without restating the fixtures-dir lookup at every call site.
#pragma once

#include "backends/MockBlueprintReader.h"

#include <filesystem>
#include <memory>

#if defined(_WIN32)
	#include <windows.h>
#endif    // defined(_WIN32)

namespace bpr::test {

inline std::filesystem::path TestExecutableDir() {
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n == MAX_PATH)
	{
		return std::filesystem::current_path();
	}
	return std::filesystem::path(buf).parent_path();
#else    // defined(_WIN32)
	return std::filesystem::current_path();
#endif    // defined(_WIN32)
}

inline std::filesystem::path FixturesDir() {
	return TestExecutableDir() / "fixtures";
}

// Most tests want a mock reader pointed at the staged fixtures. These
// factories tighten the call sites and centralize the fixtures-dir
// lookup so any future signature change to MockBlueprintReader
// touches exactly one spot.
inline backends::MockBlueprintReader MakeMockReader() {
	return backends::MockBlueprintReader(FixturesDir());
}

inline std::unique_ptr<backends::MockBlueprintReader> MakeMockReaderUnique() {
	return std::make_unique<backends::MockBlueprintReader>(FixturesDir());
}

// C4 helper: extract the `results` array from a paginated envelope, or return
// the body itself if it's already a plain array.  Lets tests written for the
// bare-array shape work transparently with the new paginated envelope:
//
//   auto out = f.Call("list_blueprints", ...);
//   auto& rows = AsResults(out);  // works whether out is array or envelope
//   REQUIRE(rows.is_array());
//   CHECK(rows.size() > 0);
//
inline const nlohmann::json& AsResults(const nlohmann::json& body) {
	if (body.is_object() && body.contains("results") && body.at("results").is_array()) {
		return body.at("results");
	}
	return body;
}
inline nlohmann::json& AsResults(nlohmann::json& body) {
	if (body.is_object() && body.contains("results") && body.at("results").is_array()) {
		return body.at("results");
	}
	return body;
}

}    // namespace bpr::test
