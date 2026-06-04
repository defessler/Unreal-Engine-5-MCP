// Unit tests for DaemonWatchdog::ShouldForceExit — the pure timing function
// behind the H3 off-game-thread watchdog. No UE runtime needed.

#include <doctest/doctest.h>
#include "DaemonWatchdog.h"
using bpr::diag::ShouldForceExit;

TEST_CASE("ShouldForceExit: inactive (0 config) never fires") {
    CHECK_FALSE(ShouldForceExit(1000, 900, 500, 0, 0));
}

TEST_CASE("ShouldForceExit: max-lifetime fires after the configured wall-clock seconds") {
    const int64_t start = 1000;
    const int32_t cap   = 300;
    CHECK_FALSE(ShouldForceExit(start + cap - 1, 0, start, cap, 0));
    CHECK(      ShouldForceExit(start + cap,     0, start, cap, 0));
    CHECK(      ShouldForceExit(start + cap + 1, 0, start, cap, 0));
}

TEST_CASE("ShouldForceExit: max-lifetime disabled when startedAtUnix==0") {
    CHECK_FALSE(ShouldForceExit(99999, 0, 0, 300, 0));
}

TEST_CASE("ShouldForceExit: wedge-detection fires after missed heartbeat threshold") {
    const int64_t lastTick = 500;
    const int32_t wedge    = 120;
    CHECK_FALSE(ShouldForceExit(lastTick + wedge - 1, lastTick, 0, 0, wedge));
    CHECK(      ShouldForceExit(lastTick + wedge,     lastTick, 0, 0, wedge));
    CHECK(      ShouldForceExit(lastTick + wedge + 1, lastTick, 0, 0, wedge));
}

TEST_CASE("ShouldForceExit: wedge not triggered when lastTickUnix==0 (never ticked)") {
    // A brand-new daemon that hasn't completed its first game-thread loop yet
    // should not be condemned immediately.
    CHECK_FALSE(ShouldForceExit(9999, 0, 1000, 0, 120));
}

TEST_CASE("ShouldForceExit: max-lifetime takes precedence over healthy heartbeat") {
    const int64_t start   = 1000;
    const int64_t lastTick= 1090;   // bumped 10s ago — healthy
    CHECK(ShouldForceExit(start + 301, lastTick, start, 300, 120));
}

TEST_CASE("ShouldForceExit: both criteria off — never fires") {
    CHECK_FALSE(ShouldForceExit(99999, 99990, 1000, 0, 0));
}
