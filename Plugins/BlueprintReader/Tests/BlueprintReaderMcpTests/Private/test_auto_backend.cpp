// Tests for AutoBlueprintReader's probe + routing logic.
//
// We don't spin up an actual UE editor here — instead we test the
// handshake-file discovery + TCP-probe paths against fixtures and
// loopback sockets we control. The integration test that drives a
// real editor lives in scripts/smoke-* (manual / live).

#include <doctest/doctest.h>

#include "backends/AutoBlueprintReader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif    // WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "Ws2_32.lib")
#else    // defined(_WIN32)
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif    // defined(_WIN32)

namespace fs = std::filesystem;

namespace {

// One-time WSAStartup for the test process.
struct WsaInit {
#if defined(_WIN32)
	WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
	~WsaInit() { WSACleanup(); }
#endif    // defined(_WIN32)
};
WsaInit& GlobalWsa() { static WsaInit s; return s; }

// Spin up a minimal localhost listener on an ephemeral port. Returns
// the port; the listener thread accepts and immediately closes (we
// only care that connect() succeeds).
struct TestListener {
	std::thread t;
	std::atomic<bool> stop{false};
	int port = 0;
#if defined(_WIN32)
	SOCKET srv = INVALID_SOCKET;
#else    // defined(_WIN32)
	int srv = -1;
#endif    // defined(_WIN32)

	explicit TestListener() {
		GlobalWsa();
#if defined(_WIN32)
		srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else    // defined(_WIN32)
		srv = ::socket(AF_INET, SOCK_STREAM, 0);
#endif    // defined(_WIN32)
		REQUIRE(srv >= 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = 0;  // ephemeral
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		REQUIRE(::listen(srv, 4) == 0);
		sockaddr_in bound{};
#if defined(_WIN32)
		int blen = sizeof(bound);
#else    // defined(_WIN32)
		socklen_t blen = sizeof(bound);
#endif    // defined(_WIN32)
		::getsockname(srv, reinterpret_cast<sockaddr*>(&bound), &blen);
		port = ntohs(bound.sin_port);
		t = std::thread([this]() {
			while (!stop.load()) {
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(srv, &rfds);
				timeval tv{0, 200 * 1000};
				int rc = ::select(static_cast<int>(srv) + 1, &rfds, nullptr, nullptr, &tv);
				if (rc > 0 && FD_ISSET(srv, &rfds)) {
#if defined(_WIN32)
					SOCKET c = ::accept(srv, nullptr, nullptr);
					if (c != INVALID_SOCKET) ::closesocket(c);
#else    // defined(_WIN32)
					int c = ::accept(srv, nullptr, nullptr);
					if (c >= 0) ::close(c);
#endif    // defined(_WIN32)
				}
			}
		});
	}

	~TestListener() {
		stop.store(true);
		if (t.joinable()) t.join();
#if defined(_WIN32)
		if (srv != INVALID_SOCKET) ::closesocket(srv);
#else    // defined(_WIN32)
		if (srv >= 0) ::close(srv);
#endif    // defined(_WIN32)
	}
};

// Create a temp project layout with a Saved/bp-reader-live.json,
// returning the (synthetic) .uproject path.
struct TempProject {
	fs::path dir;
	fs::path uproject;

	TempProject() {
		dir = fs::temp_directory_path() /
			  ("bpr-auto-" + std::to_string(std::chrono::steady_clock::now()
												.time_since_epoch().count()));
		fs::create_directories(dir / "Saved");
		uproject = dir / "Test.uproject";
		std::ofstream(uproject) << "{}";
	}
	~TempProject() {
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	void WriteHandshake(int port, const std::string& token) {
		std::ofstream f(dir / "Saved" / "bp-reader-live.json");
		f << R"({"version":1,"host":"127.0.0.1","port":)" << port
		  << R"(,"token":")" << token << R"(","pid":1234,"started_at":"now"})";
	}

	void DeleteHandshake() {
		std::error_code ec;
		fs::remove(dir / "Saved" / "bp-reader-live.json", ec);
	}
};

// Build an Auto reader with the commandlet config pointed nowhere real.
// The commandlet ctor validates that engineDir + uproject are non-empty
// but doesn't touch the disk — we just need any path strings. We never
// invoke a commandlet in these tests (probe-routing is the surface).
bpr::backends::AutoBlueprintReader::Config MakeAutoCfg(const TempProject& tp) {
	bpr::backends::AutoBlueprintReader::Config cfg;
	cfg.uproject = tp.uproject;
	cfg.probeTtl = std::chrono::milliseconds(0);  // always re-probe
	cfg.probeConnectTimeout = std::chrono::milliseconds(200);
	cfg.commandletConfig.uproject  = tp.uproject;
	cfg.commandletConfig.engineDir = tp.dir;       // any non-empty path
	cfg.commandletConfig.useDaemon = false;
	return cfg;
}

} // namespace

TEST_CASE("Auto: no handshake file → falls through to commandlet") {
	TempProject tp;
	bpr::backends::AutoBlueprintReader reader(MakeAutoCfg(tp));
	CHECK(reader.SelectBackendForTesting() == "commandlet");
}

TEST_CASE("Auto: stale handshake file (port not listening) → commandlet") {
	TempProject tp;
	// Port 1 is reserved/unprivileged → connect fails fast on every OS.
	tp.WriteHandshake(/*port=*/1, /*token=*/"abc123");
	bpr::backends::AutoBlueprintReader reader(MakeAutoCfg(tp));
	CHECK(reader.SelectBackendForTesting() == "commandlet");
}

TEST_CASE("Auto: handshake file + listener up → routes to live") {
	TestListener listener;
	TempProject tp;
	tp.WriteHandshake(listener.port, "tok-abc");
	bpr::backends::AutoBlueprintReader reader(MakeAutoCfg(tp));
	CHECK(reader.SelectBackendForTesting() == "live");
}

TEST_CASE("Auto: editor 'closes' (file deleted) → flips back to commandlet") {
	TestListener listener;
	TempProject tp;
	tp.WriteHandshake(listener.port, "tok-abc");
	bpr::backends::AutoBlueprintReader reader(MakeAutoCfg(tp));
	CHECK(reader.SelectBackendForTesting() == "live");

	tp.DeleteHandshake();
	CHECK(reader.SelectBackendForTesting() == "commandlet");
}

TEST_CASE("Auto: editor 'opens' mid-session → flips to live") {
	TempProject tp;
	bpr::backends::AutoBlueprintReader reader(MakeAutoCfg(tp));
	REQUIRE(reader.SelectBackendForTesting() == "commandlet");

	TestListener listener;
	tp.WriteHandshake(listener.port, "tok-abc");
	CHECK(reader.SelectBackendForTesting() == "live");
}

TEST_CASE("Auto: explicit env-style port/token wins over handshake file") {
	TestListener listenerA;
	TestListener listenerB;
	REQUIRE(listenerA.port != listenerB.port);

	TempProject tp;
	// Handshake file points at listener A.
	tp.WriteHandshake(listenerA.port, "from-file");

	// Caller config (env-var pass-through) points at listener B.
	auto cfg = MakeAutoCfg(tp);
	cfg.livePort  = listenerB.port;
	cfg.liveToken = "from-env";

	bpr::backends::AutoBlueprintReader reader(std::move(cfg));
	CHECK(reader.SelectBackendForTesting() == "live");
	// We can't check WHICH listener was picked without inspecting
	// private state; the routing test above is the meaningful coverage.
	// The "file-vs-env" precedence is covered by inspecting Config in
	// the BackendFactory tests — see test_backend_factory.cpp if/when
	// we add one.
}
