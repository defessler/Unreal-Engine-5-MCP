// Inline helper shared between CommandletBlueprintReader and its unit
// tests. Kept in a header (rather than the anonymous namespace inside
// the .cpp) so the encoder can be exercised directly from doctest
// without bringing in a live UE editor.
//
// Encodes a single `-Key=Value` commandlet arg so UE's FParse::Value
// reads the full value even when it contains whitespace (pin names
// like "Dummy Targets", comment text, default-value strings — fixes
// issue #10). Strategy:
//
//   - Args without an `=` (flags like `-Compact`) pass through.
//   - Values already wrapped in `"..."` by the caller pass through.
//   - Values with no whitespace and no `"` pass through bare.
//   - Otherwise emit `-Key="Value"`. FParse's quoted-value path
//     triggers when the char immediately after `=` is `"`.
//
// FParse has no backslash-escape mechanism, so we cannot embed literal
// `"` inside the quoted value — any inner `"` would terminate the
// parse early. The encoder strips inner quotes defensively; the wire
// shapes we send (pin names, asset paths, GUIDs, etc.) don't contain
// `"` in practice.
//
// The `-Key="..."` form is also safe in a CreateProcessW lpCmdLine —
// the child's GetCommandLineW returns the raw string verbatim and UE
// FParse on that string still sees the inner quotes. We deliberately
// do NOT apply Windows-style outer escaping (no `\"`) because that
// would defeat FParse's quote detection. The exe path still goes
// through normal Windows QuoteArg in BuildCommandLine.

#pragma once

#include <string>
#include <string_view>

namespace bpr::backends::detail {

inline std::wstring EncodeArgForFParse(std::wstring_view arg) {
	auto eq = arg.find(L'=');
	if (eq == std::wstring_view::npos) {
		return std::wstring(arg);
	}
	std::wstring_view key   = arg.substr(0, eq);   // includes leading "-"
	std::wstring_view value = arg.substr(eq + 1);
	if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
		return std::wstring(arg);
	}
	if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(arg);
	}
	std::wstring out;
	out.reserve(arg.size() + 2);
	out.append(key);
	out.push_back(L'=');
	out.push_back(L'"');
	for (wchar_t c : value) {
		if (c == L'"') continue;  // FParse has no escape; strip defensively
		out.push_back(c);
	}
	out.push_back(L'"');
	return out;
}

// CommandLineToArgvW round-trip safe quoting per Microsoft's parsing
// rules. Used for positional args (e.g. the uproject path) where UE
// doesn't run FParse against the inner content — the OS just needs
// each argv entry to come through whole.
inline std::wstring QuoteWindowsArg(std::wstring_view in) {
	if (!in.empty() && in.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(in);
	}
	std::wstring out;
	out.reserve(in.size() + 2);
	out.push_back(L'"');
	for (std::size_t i = 0; i < in.size();) {
		std::size_t backslashes = 0;
		while (i < in.size() && in[i] == L'\\') { ++backslashes; ++i; }
		if (i == in.size()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (in[i] == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
			++i;
		} else {
			out.append(backslashes, L'\\');
			out.push_back(in[i]);
			++i;
		}
	}
	out.push_back(L'"');
	return out;
}

// Dispatcher: pick the right quoting strategy per arg. UE's FParse::Value
// only kicks in for `-Key=Value` flag args, so inner-quote those for
// whitespace values (issue #10). Everything else — positional paths,
// bare flags — needs standard Windows outer quoting so the
// CreateProcessW → CommandLineToArgvW round trip keeps the arg as one
// argv entry.
//
// Detection is "starts with `-` AND contains `="; just checking for `=`
// alone misroutes positional paths whose name happens to include `=`
// (e.g. `C:\Foo=Unreal Projects\Game.uproject`, which is a legal NTFS
// path). Codex review on PR #61 caught that edge case — the original
// dispatcher would have split it on the embedded `=` as if it were a
// key/value pair and broken commandlet launch for that directory.
//
// History:
//   PR #52 original: ALL args → FParse encoder → broke positional paths
//     with spaces (`C:\Unreal Projects\Foo.uproject`)
//   PR #61 fixup: `=` switch → broke positional paths with literal `=`
//   This commit: `-` prefix + `=` switch → handles both
inline std::wstring EncodeArg(std::wstring_view arg) {
	const bool bLooksLikeOption =
		!arg.empty() && arg.front() == L'-' &&
		arg.find(L'=') != std::wstring_view::npos;
	if (bLooksLikeOption) {
		return EncodeArgForFParse(arg);
	}
	return QuoteWindowsArg(arg);
}

} // namespace bpr::backends::detail
