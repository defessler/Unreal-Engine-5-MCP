// DaemonWatchdog.h — off-game-thread daemon liveness logic (H3).
//
// The timing decision is a pure function of wall-clock values so it can be
// unit-tested in the standalone CI suite (no UE runtime needed). The actual
// watchdog FRunnable that calls it lives in BlueprintReaderCmdletServer.cpp.
//
// Parameters
//   nowUnix         current wall-clock unix timestamp (seconds)
//   lastTickUnix    last time the game thread bumped its heartbeat counter
//   startedAtUnix   when the daemon process started (0 = disabled)
//   maxLifetimeSecs hard max lifetime in seconds (0 = disabled)
//   wedgeSecs       how long a silent game thread is tolerated before force-exit
//
// Returns true when the watchdog should force-exit the daemon process.
#pragma once
#include <cstdint>

namespace bpr::diag {

inline bool ShouldForceExit(int64_t nowUnix,
							int64_t lastTickUnix,
							int64_t startedAtUnix,
							int32_t maxLifetimeSecs,
							int32_t wedgeSecs) noexcept {
	// Max-lifetime: force-exit after the configured wall-clock ceiling,
	// independent of whether the game thread is responsive or not.
	if (maxLifetimeSecs > 0 && startedAtUnix > 0) {
		if (nowUnix - startedAtUnix >= static_cast<int64_t>(maxLifetimeSecs)) {
			return true;
		}
	}
	// Heartbeat: if the game thread stopped bumping its counter (e.g. it is
	// stuck in a long compile / UE asset save) for more than wedgeSecs, the
	// daemon can never answer future requests and will never idle-shutdown on
	// its own (WantsShutdown() is polled from the wedged game thread).
	// lastTickUnix == 0 means the daemon has never completed a game-thread
	// loop iteration — give it a generous grace period before condemning it.
	if (wedgeSecs > 0 && lastTickUnix > 0) {
		if (nowUnix - lastTickUnix >= static_cast<int64_t>(wedgeSecs)) {
			return true;
		}
	}
	return false;
}

}  // namespace bpr::diag
