// Unit tests for EncodeArgForFParse — the small helper that inner-quotes
// `-Key=Value` commandlet args containing whitespace so UE's
// FParse::Value reads the full value (issue #10: wire_pins fails when
// the pin name has spaces, like "Dummy Targets"). These tests don't
// need a UE editor — they exercise the string transform in isolation.

#include <doctest/doctest.h>

#include "backends/CommandletArgEncoding.h"

using bpr::backends::detail::EncodeArgForFParse;

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
