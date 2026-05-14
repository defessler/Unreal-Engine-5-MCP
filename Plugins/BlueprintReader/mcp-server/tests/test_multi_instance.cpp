// Two MockBlueprintReader instances share fixtures without locking.
// Pins "the MCP server is no longer single-instance" at the test level
// so a future regression of the lock is caught. Pure mock backend —
// no socket / daemon involved.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "test_helpers.h"

using namespace bpr::backends;

TEST_CASE("Two MockBlueprintReader instances share fixtures without locking") {
    auto reader1 = bpr::test::MakeMockReader();
    auto reader2 = bpr::test::MakeMockReader();

    auto a = reader1.ListBlueprints("/Game");
    auto b = reader2.ListBlueprints("/Game");

    CHECK(a.size() == b.size());
    CHECK(a.size() > 0);
}
