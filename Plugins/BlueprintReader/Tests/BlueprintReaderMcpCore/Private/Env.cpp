#include "Env.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif    // defined(_WIN32)

namespace bpr::env {

std::optional<std::string> Get(const char* key) {
#ifdef _MSC_VER
	char* buf = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
		std::string out(buf);
		std::free(buf);
		if (out.empty())
		{
			return std::nullopt;
		}
		return out;
	}
	return std::nullopt;
#else    // !_MSC_VER
	if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
		return std::string(v);
	}
	return std::nullopt;
#endif    // _MSC_VER
}

std::string GetOrDefault(const char* key, std::string fallback) {
	if (auto v = Get(key))
	{
		return *v;
	}
	return fallback;
}

bool BoolOrDefault(const char* key, bool fallback, std::ostream& log) {
	auto v = Get(key);
	if (!v)
	{
		return fallback;
	}
	std::string lower;
	lower.reserve(v->size());
	for (char c : *v)
	{
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
	{
		return true;
	}
	if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
	{
		return false;
	}
	log << "[bp-reader-mcp] warning: " << key << "=" << *v
		<< " not understood (expected 0/1/true/false/yes/no/on/off); using default "
		<< (fallback ? "true" : "false") << "\n";
	return fallback;
}

int IntOrDefault(const char* key, int fallback) {
	auto v = Get(key);
	if (!v)
	{
		return fallback;
	}
	try {
		return std::stoi(*v);
	} catch (...) {
		// REL-24: a typo'd numeric env var (BP_READER_TIMEOUT_SECONDS=abc)
		// silently used the default — warn so the misconfiguration is
		// visible instead of mystifying.
		std::fprintf(stderr,
			"[bp-reader-mcp] warning: %s=%s is not a number; using default %d\n",
			key, v->c_str(), fallback);
		return fallback;
	}
}

bool VerboseLoggingEnabled() {
	// Cached on first call. Read directly (don't reuse BoolOrDefault)
	// because BoolOrDefault writes to an ostream, and we don't have one
	// — plus this is supposed to stay silent unless explicitly enabled.
	static const bool kEnabled = [] {
		auto v = Get("BP_READER_VERBOSE");
		if (!v) {
			return false;
		}
		std::string lower;
		lower.reserve(v->size());
		for (char c : *v) {
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
	}();
	return kEnabled;
}

bool NeverInlineImages() {
	// Phase D kill switch — when set, the screenshot tools ignore the
	// `return_inline=true` arg and always write to disk only. Lets a
	// security-conscious deployment force-disable image-in-response paths
	// without rebuilding. Cached for the same reason as
	// VerboseLoggingEnabled — env vars don't change at runtime.
	static const bool kEnabled = [] {
		auto v = Get("BP_READER_NEVER_INLINE_IMAGES");
		if (!v) {
			return false;
		}
		std::string lower;
		lower.reserve(v->size());
		for (char c : *v) {
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
	}();
	return kEnabled;
}

// ---------------------------------------------------------------------------
// Auto-discovery
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path>
FindUprojectAbove(const std::filesystem::path& start, int maxDepth) {
	std::error_code ec;
	auto current = std::filesystem::absolute(start, ec);
	if (ec)
	{
		return std::nullopt;
	}
	for (int i = 0; i < maxDepth; ++i) {
		if (!std::filesystem::is_directory(current, ec)) {
			current = current.parent_path();
			if (current.empty())
			{
				return std::nullopt;
			}
			continue;
		}
		// Look for exactly one *.uproject in this dir.
		std::filesystem::path found;
		int count = 0;
		for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
			if (ec)
			{
				break;
			}
			if (entry.is_regular_file(ec) && entry.path().extension() == ".uproject") {
				found = entry.path();
				if (++count > 1)
				{
					break;
				}
			}
		}
		if (count == 1)
		{
			return found;
		}
		// Walk up.
		auto parent = current.parent_path();
		if (parent == current)
		{
			return std::nullopt;
		}
		current = parent;
	}
	return std::nullopt;
}

std::optional<std::string>
ReadEngineAssociation(const std::filesystem::path& uproject) {
	try {
		std::ifstream in(uproject);
		if (!in)
		{
			return std::nullopt;
		}
		nlohmann::json j;
		in >> j;
		auto it = j.find("EngineAssociation");
		if (it != j.end() && it->is_string()) {
			auto s = it->get<std::string>();
			return s.empty() ? std::nullopt : std::optional<std::string>(s);
		}
	} catch (...) {
		// Malformed .uproject — fall through to nullopt.
	}
	return std::nullopt;
}

std::optional<std::filesystem::path>
ResolveEngineFromRegistry(const std::string& engineAssociation) {
#if defined(_WIN32)
	HKEY h = nullptr;
	if (RegOpenKeyExA(HKEY_CURRENT_USER,
					  "SOFTWARE\\Epic Games\\Unreal Engine\\Builds",
					  0, KEY_READ, &h) != ERROR_SUCCESS) {
		return std::nullopt;
	}
	char buf[1024];
	DWORD bufLen = sizeof(buf);
	DWORD type = 0;
	auto rc = RegQueryValueExA(h, engineAssociation.c_str(), nullptr, &type,
							   reinterpret_cast<BYTE*>(buf), &bufLen);
	RegCloseKey(h);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
		bufLen == 0) {
		return std::nullopt;
	}
	// bufLen includes the trailing NUL, sometimes.
	std::string s(buf, (bufLen > 0 && buf[bufLen - 1] == '\0') ? bufLen - 1 : bufLen);
	std::filesystem::path p(s);
	std::error_code ec;
	if (!std::filesystem::is_directory(p, ec))
	{
		return std::nullopt;
	}
	return p;
#else    // defined(_WIN32)
	(void)engineAssociation;
	return std::nullopt;
#endif    // defined(_WIN32)
}

std::optional<std::string>
DetectEditorConfig(const std::filesystem::path& pluginDir) {
	auto binDir = pluginDir / "Binaries" / "Win64";
	std::error_code ec;
	if (!std::filesystem::is_directory(binDir, ec))
	{
		return std::nullopt;
	}

	// Prefer suffix-less Development if it exists — that's what the daemon
	// launches by default.
	auto dev = binDir / "UnrealEditor-BlueprintReaderEditor.dll";
	if (std::filesystem::exists(dev, ec))
	{
		return std::string("Development");
	}

	// Otherwise pick whichever -Win64-<Config>.dll variant exists.
	static const std::regex re(
		R"(^UnrealEditor-BlueprintReaderEditor-Win64-([A-Za-z]+)\.dll$)");
	for (const auto& entry : std::filesystem::directory_iterator(binDir, ec)) {
		if (ec)
		{
			break;
		}
		if (!entry.is_regular_file(ec))
		{
			continue;
		}
		auto name = entry.path().filename().string();
		std::smatch m;
		if (std::regex_match(name, m, re)) {
			return m[1].str();
		}
	}
	return std::nullopt;
}

}    // namespace bpr::env
