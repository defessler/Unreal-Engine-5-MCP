// Unit tests for EncodeArgForFParse — the small helper that inner-quotes
// `-Key=Value` commandlet args containing whitespace so UE's
// FParse::Value reads the full value (issue #10: wire_pins fails when
// the pin name has spaces, like "Dummy Targets"). These tests don't
// need a UE editor — they exercise the string transform in isolation.

#include <doctest/doctest.h>

#include "backends/CommandletArgEncoding.h"

using bpr::backends::detail::EncodeArg;
using bpr::backends::detail::EncodeArgForFParse;
using bpr::backends::detail::QuoteWindowsArg;

TEST_CASE("EncodeArgForFParse: flag without '=' passes through") {
    CHECK(EncodeArgForFParse(L"-Compact") == L"-Compact");
    CHECK(EncodeArgForFParse(L"-Daemon") == L"-Daemon");
}

TEST_CASE("EncodeArgForFParse: value without whitespace passes through bare") {
    CHECK(EncodeArgForFParse(L"-Asset=/Game/AI/BP_TestEnemy") ==
          L"-Asset=/Game/AI/BP_TestEnemy");
    CHECK(EncodeArgForFParse(L"-Graph=EventGraph") == L"-Graph=EventGraph");
    CHECK(EncodeArgForFParse(L"-FromNode=01234567-89AB-CDEF-0123-456789ABCDEF") ==
          L"-FromNode=01234567-89AB-CDEF-0123-456789ABCDEF");
}

TEST_CASE("EncodeArgForFParse: value with spaces gets inner-quoted") {
    // The bug from issue #10: pin names like "Dummy Targets" need
    // inner quotes so FParse::Value's quoted-value reader kicks in.
    CHECK(EncodeArgForFParse(L"-FromPin=Dummy Targets") ==
          L"-FromPin=\"Dummy Targets\"");
    CHECK(EncodeArgForFParse(L"-ToPin=Cast To Pawn") ==
          L"-ToPin=\"Cast To Pawn\"");
    CHECK(EncodeArgForFParse(L"-Comment=Some explanatory note") ==
          L"-Comment=\"Some explanatory note\"");
}

TEST_CASE("EncodeArgForFParse: value with tabs/newlines also gets inner-quoted") {
    CHECK(EncodeArgForFParse(L"-Comment=line1\tcol2") ==
          L"-Comment=\"line1\tcol2\"");
    CHECK(EncodeArgForFParse(L"-Comment=line1\nline2") ==
          L"-Comment=\"line1\nline2\"");
}

TEST_CASE("EncodeArgForFParse: value with embedded quotes has them stripped") {
    // FParse has no backslash-escape mechanism, so inner `"` in the
    // value would terminate the parse early. The encoder strips them
    // defensively. This is fine in practice — the wire shapes we send
    // (pin names, asset paths, GUIDs) can't legally contain `"`.
    CHECK(EncodeArgForFParse(L"-Pin=Some \"weird\" name") ==
          L"-Pin=\"Some weird name\"");
}

TEST_CASE("EncodeArgForFParse: already inner-quoted value passes through unchanged") {
    // Idempotency check: encoding a pre-encoded arg shouldn't double-
    // wrap or break it.
    auto once  = EncodeArgForFParse(L"-FromPin=Dummy Targets");
    auto twice = EncodeArgForFParse(once);
    CHECK(once == twice);
    CHECK(twice == L"-FromPin=\"Dummy Targets\"");
}

TEST_CASE("EncodeArgForFParse: empty value passes through (FParse handles it)") {
    CHECK(EncodeArgForFParse(L"-Query=") == L"-Query=");
}

TEST_CASE("EncodeArgForFParse: key with no leading '-' still works") {
    // Helper doesn't assume the `-` prefix — it's purely a split-at-=
    // transform. The plugin call sites all include `-`, but the
    // encoder is robust either way.
    CHECK(EncodeArgForFParse(L"Foo=Bar Baz") == L"Foo=\"Bar Baz\"");
}

// ---- EncodeArg dispatcher (PR #52 Codex follow-up) -----------------------
// The dispatcher routes -Key=Value args through FParse-style inner quoting
// (so UE's FParse::Value reads whitespace values intact) and positional
// args through standard Windows outer quoting (so CommandLineToArgvW
// keeps paths like `C:\Unreal Projects\Foo.uproject` whole). Codex
// flagged the original PR #52 where ALL args went through the FParse
// encoder, splitting positional project paths on spaces.

TEST_CASE("EncodeArg: positional path with spaces gets Windows outer quoting") {
    // The uproject path is the first positional arg after the exe and
    // contains no `=`. It MUST come out wrapped in outer quotes so
    // CommandLineToArgvW round-trips it as one argv entry.
    CHECK(EncodeArg(L"C:\\Unreal Projects\\Foo.uproject") ==
          L"\"C:\\Unreal Projects\\Foo.uproject\"");
}

TEST_CASE("EncodeArg: positional path without spaces passes through bare") {
    CHECK(EncodeArg(L"C:\\Projects\\Foo.uproject") ==
          L"C:\\Projects\\Foo.uproject");
}

TEST_CASE("EncodeArg: -Key=Value with whitespace still uses inner quoting") {
    // Same path as EncodeArgForFParse for `=`-bearing args — verify the
    // dispatcher doesn't accidentally route them through QuoteWindowsArg
    // (which would produce `\"-FromPin=Dummy Targets\"` and break FParse).
    CHECK(EncodeArg(L"-FromPin=Dummy Targets") ==
          L"-FromPin=\"Dummy Targets\"");
}

TEST_CASE("EncodeArg: bare flag without '=' or whitespace passes through") {
    // -Daemon, -nullrhi, etc. — no `=`, no spaces, no quoting needed.
    CHECK(EncodeArg(L"-Daemon") == L"-Daemon");
    CHECK(EncodeArg(L"-nullrhi") == L"-nullrhi");
}

TEST_CASE("EncodeArg: positional path with literal '=' still gets Windows quoting") {
    // Codex review on PR #61 caught this: the dispatcher used to look
    // only for `=` to decide FParse-vs-Windows. A legal NTFS path like
    // `C:\Foo=Unreal Projects\Game.uproject` contains both `=` and a
    // space, so the old logic split it as if it were a key/value pair
    // and emitted `C:\Foo="Unreal Projects\Game.uproject"`, breaking
    // commandlet launch. Detection now also requires a leading `-`.
    CHECK(EncodeArg(L"C:\\Foo=Unreal Projects\\Game.uproject") ==
          L"\"C:\\Foo=Unreal Projects\\Game.uproject\"");
    // Also: positional path with `=` but no spaces stays bare.
    CHECK(EncodeArg(L"C:\\Foo=Bar\\Game.uproject") ==
          L"C:\\Foo=Bar\\Game.uproject");
}

TEST_CASE("EncodeArgForFParse: full WirePins arg line round-trips via UE FParse") {
    // Integration check: the daemon-line writer (CommandletBlueprintReader::
    // RunOpDaemon) builds a single newline-terminated line by joining all
    // op args with single spaces and feeding it to the daemon's stdin. The
    // daemon then hands the whole line to UE FParse::Value, which extracts
    // each `-Key=` value via case-insensitive substring search.
    //
    // This test simulates the joined line + the FParse pass without any UE
    // dependency. The key invariant is that values with whitespace survive
    // the join + the (simulated) FParse::Value scan with their full content
    // intact.
    auto encode = [](std::wstring_view a) {
        return EncodeArgForFParse(a);
    };
    std::wstring line =
        encode(L"-Op=WirePins") + L" " +
        encode(L"-Asset=/Game/AI/BP_TestEnemy") + L" " +
        encode(L"-Graph=EventGraph") + L" " +
        encode(L"-FromNode=11111111-2222-3333-4444-555555555555") + L" " +
        encode(L"-FromPin=Dummy Targets") + L" " +  // <-- the issue #10 case
        encode(L"-ToNode=66666666-7777-8888-9999-aaaaaaaaaaaa") + L" " +
        encode(L"-ToPin=Target Array");             // <-- another spaced name

    // The relevant substring shapes (what FParse::Value will see) must
    // appear in the joined line. We deliberately check the inner-quoted
    // form (the only form FParse's quoted-value reader accepts).
    CHECK(line.find(L"-FromPin=\"Dummy Targets\"") != std::wstring::npos);
    CHECK(line.find(L"-ToPin=\"Target Array\"") != std::wstring::npos);
    // And no-whitespace args stay bare — no spurious outer quoting that
    // would slip past FParse and end up in the extracted value.
    CHECK(line.find(L"-Op=WirePins ") != std::wstring::npos);
    CHECK(line.find(L"-Asset=/Game/AI/BP_TestEnemy ") != std::wstring::npos);

    // Simulate UE FParse::Value behavior on the joined line. UE's actual
    // implementation: locate `Match` substring, advance past `=`, if next
    // char is `"` read until matching `"`, else read until whitespace.
    auto fparseValue = [&](std::wstring_view stream, std::wstring_view match) -> std::wstring {
        auto pos = stream.find(match);
        if (pos == std::wstring_view::npos) return {};
        auto start = pos + match.size();
        if (start < stream.size() && stream[start] == L'"') {
            auto end = stream.find(L'"', start + 1);
            if (end == std::wstring_view::npos) end = stream.size();
            return std::wstring(stream.substr(start + 1, end - start - 1));
        }
        auto end = stream.find_first_of(L" \t\r\n", start);
        if (end == std::wstring_view::npos) end = stream.size();
        return std::wstring(stream.substr(start, end - start));
    };
    CHECK(fparseValue(line, L"FromPin=") == L"Dummy Targets");
    CHECK(fparseValue(line, L"ToPin=")   == L"Target Array");
    CHECK(fparseValue(line, L"Asset=")   == L"/Game/AI/BP_TestEnemy");
}
