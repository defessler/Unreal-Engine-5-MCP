// Two-lock spawn coordination (Task 4.1).
//
// Verifies that when two CommandletBlueprintReader instances (modeling
// two separate MCP-server processes) race to start a daemon, the
// bp-reader-cmdlet-spawn.lock collapses the race down to ONE spawn.
// The loser of the spawn-lock race waits for the winner's handshake
// instead of double-launching UnrealEditor-Cmd.exe.
//
// The test injects a `spawnDaemonHook` into the Config that:
//   1. Increments an atomic spawn counter (the assertion).
//   2. Sleeps to simulate spawn latency.
//   3. Writes a handshake JSON file pointing at a real in-test TCP
//      listener so the subsequent TryAttachExistingDaemon TCP probe
//      succeeds (the second-arriver thread needs a live listener to
//      attach to; otherwise it'd spuriously fall back to spawning).
//
// Only meaningful on Windows — the SpawnLock is a Win32-only construct
// (the daemon ships Windows-only today).

#include <doctest/doctest.h>

#include "backends/CommandletBlueprintReader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <windows.h>
	#pragma comment(lib, "Ws2_32.lib")
#endif

using namespace bpr::backends;
using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)

struct WsaInit {
	WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
	~WsaInit() { WSACleanup(); }
};
WsaInit& Wsa() { static WsaInit s; return s; }

// Minimal accept-and-hold TCP listener used so the TCP probe in
// TryAttachExistingDaemon succeeds for both racing threads. We don't
// run the full handshake here — the subsequent SocketBlueprintReader
// op will fail (no auth response), which is fine: the test only cares
// about spawn-coordination, not end-to-end attach.
class StubListener {
public:
	StubListener() {
		Wsa();
		listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		REQUIRE(listenSock_ != INVALID_SOCKET);
		sockaddr_in addr{};
		addr.sin_family      = AF_INET;
		addr.sin_port        = 0;  // ephemeral
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == 0);
		int addrLen = sizeof(addr);
		REQUIRE(::getsockname(listenSock_, (sockaddr*)&addr, &addrLen) == 0);
		port_ = ntohs(addr.sin_port);
		REQUIRE(::listen(listenSock_, SOMAXCONN) == 0);

		// Accept any inbound connection and immediately close it. The
		// probe in TryAttachExistingDaemon only checks that connect()
		// succeeds; it doesn't perform the protocol handshake itself.
		thread_ = std::thread([this] {
			while (!stop_.load()) {
				SOCKET c = ::accept(listenSock_, nullptr, nullptr);
				if (c == INVALID_SOCKET) return;
				::closesocket(c);
			}
		});
	}
	~StubListener() {
		stop_.store(true);
		::closesocket(listenSock_);
		if (thread_.joinable()) thread_.join();
	}
	int port() const { return port_; }
private:
	SOCKET listenSock_;
	int port_ = 0;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

TEST_CASE("CommandletBackend: simultaneous spawn attempts coalesce to one") {
	std::atomic<int> spawnCount{0};

	// Isolate per-test in a unique temp dir to avoid colliding with any
	// real project's Saved/ folder (and so concurrent test runs don't
	// step on each other through shared lock files).
	auto tempProjectDir = std::filesystem::temp_directory_path() /
		("bp-reader-spawn-race-" +
		 std::to_string(reinterpret_cast<std::uintptr_t>(&spawnCount)));
	std::filesystem::create_directories(tempProjectDir / "Saved");

	// CommandletBlueprintReader's ctor only checks std::filesystem::exists
	// on the uproject path — content doesn't matter, so an empty file is
	// enough. We also need editorDir to "exist" enough to pass the
	// UnrealEditor-Cmd.exe probe; point it at a path containing a fake
	// exe so construction succeeds. The spawnDaemonHook intercepts
	// before the exe is ever launched.
	auto uproject = tempProjectDir / "Fake.uproject";
	{ std::ofstream f(uproject); f << "{}"; }

	auto engineDir = tempProjectDir / "EngineFake";
	auto binDir = engineDir / "Engine" / "Binaries" / "Win64";
	std::filesystem::create_directories(binDir);
	{ std::ofstream f(binDir / "UnrealEditor-Cmd.exe"); f << ""; }

	StubListener listener;
	const int listenerPort = listener.port();

	// Fake spawn: increments the counter, then writes a handshake file
	// after a 200ms delay (simulating real spawn latency so the second
	// thread definitely contends on the spawn lock rather than coasting
	// through on a stale handshake).
	auto fakeSpawn = [&](const std::filesystem::path& proj) {
		++spawnCount;
		std::this_thread::sleep_for(200ms);
		nlohmann::json hs = {
			{"version", 1},
			{"host",    "127.0.0.1"},
			{"port",    listenerPort},
			{"token",   "fake-token"},
			{"pid",     static_cast<int>(::GetCurrentProcessId())},
		};
		std::ofstream f(proj.parent_path() / "Saved" / "bp-reader-cmdlet.json");
		f << hs.dump();
	};

	auto makeReader = [&]() {
		CommandletBlueprintReader::Config cfg;
		cfg.engineDir       = engineDir;
		cfg.uproject        = uproject;
		cfg.useDaemon       = true;
		cfg.startupTimeout  = 5s;
		cfg.spawnDaemonHook = fakeSpawn;
		return std::make_unique<CommandletBlueprintReader>(std::move(cfg));
	};

	auto reader1 = makeReader();
	auto reader2 = makeReader();

	// Race them on parallel threads. Both calls go through
	// EnsureDaemonAttached → SpawnLock contention. We don't care
	// whether the final ListBlueprints op succeeds (the stub listener
	// doesn't speak the auth protocol, so the SocketBlueprintReader
	// call will throw further downstream). We only care that the spawn
	// hook fired exactly once.
	auto t1 = std::async(std::launch::async, [&] {
		try { (void)reader1->ListBlueprints("/Game"); } catch (...) {}
	});
	auto t2 = std::async(std::launch::async, [&] {
		try { (void)reader2->ListBlueprints("/Game"); } catch (...) {}
	});
	t1.wait();
	t2.wait();

	// The crucial assertion: only ONE of the two readers invoked the
	// spawn hook. The other waited for the handshake file the first
	// one published, then attached to the same listener via
	// TryAttachExistingDaemon's TCP probe.
	CHECK(spawnCount.load() == 1);

	// Tear the readers down before nuking their temp dir so their
	// destructors release the SocketBlueprintReader handles cleanly.
	reader1.reset();
	reader2.reset();

	std::error_code ec;
	std::filesystem::remove_all(tempProjectDir, ec);
}

// Stale-handshake recovery: a handshake file from a crashed previous
// daemon (its pid no longer exists) must NOT be reused. The pid-alive
// check in TryAttachExistingDaemon should reject it, and the
// CommandletBlueprintReader should fall through to the spawn path.
TEST_CASE("CommandletBackend: stale handshake (dead pid) triggers fresh spawn") {
	std::atomic<bool> spawnCalled{false};

	auto tempProjectDir = std::filesystem::temp_directory_path() /
		("bp-reader-stale-handshake-" +
		 std::to_string(reinterpret_cast<std::uintptr_t>(&spawnCalled)));
	std::filesystem::create_directories(tempProjectDir / "Saved");

	auto uproject = tempProjectDir / "Fake.uproject";
	{ std::ofstream f(uproject); f << "{}"; }

	auto engineDir = tempProjectDir / "EngineFake";
	auto binDir = engineDir / "Engine" / "Binaries" / "Win64";
	std::filesystem::create_directories(binDir);
	{ std::ofstream f(binDir / "UnrealEditor-Cmd.exe"); f << ""; }

	// Write a handshake claiming a daemon at a (probably) unused port
	// with a pid that definitely doesn't exist on this machine
	// (max-int32, far beyond any real PID). The pid-alive check should
	// reject this and trigger fresh spawn — without ever probing the
	// port (so it doesn't matter that nothing's listening).
	nlohmann::json hs = {
		{"version", 1},
		{"host",    "127.0.0.1"},
		{"port",    65530},
		{"token",   "stale"},
		{"pid",     0x7FFFFFFE},
	};
	{
		std::ofstream f(tempProjectDir / "Saved" / "bp-reader-cmdlet.json");
		f << hs.dump();
	}

	CommandletBlueprintReader::Config cfg;
	cfg.engineDir       = engineDir;
	cfg.uproject        = uproject;
	cfg.useDaemon       = true;
	cfg.startupTimeout  = 2s;
	cfg.spawnDaemonHook = [&](const std::filesystem::path&) {
		spawnCalled.store(true);
		// Don't actually write a fresh handshake — let the test observe
		// the spawn-call without succeeding at attach. The subsequent
		// op will throw downstream, which the test swallows.
	};

	CommandletBlueprintReader reader(std::move(cfg));
	try { (void)reader.ListBlueprints("/Game"); } catch (...) {}

	CHECK(spawnCalled.load());

	std::error_code ec;
	std::filesystem::remove_all(tempProjectDir, ec);
}

#endif  // _WIN32
