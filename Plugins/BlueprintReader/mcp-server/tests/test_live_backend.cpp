// Tests for LiveBlueprintReader's wire protocol against a mock TCP
// server. Validates the handshake (hello → auth → auth_ok), the op
// frame shape, and the result-frame parsing — without needing a real
// UE editor running.

#include <doctest/doctest.h>

#include "backends/LiveBlueprintReader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

using namespace bpr::backends;
using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
WsaInit& Wsa() { static WsaInit s; return s; }
#endif

// Tiny one-shot mock listener: spins up a thread, accepts one connection,
// runs a scripted exchange, then closes. The script is a callback the
// test provides.
class MockServer {
public:
    using Script = std::function<void(SOCKET clientSock)>;

    MockServer(Script script) : script_(std::move(script)) {
        Wsa();
        listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listenSock_ != INVALID_SOCKET);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral port — kernel picks one
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == 0);
        int addrLen = sizeof(addr);
        REQUIRE(::getsockname(listenSock_, (sockaddr*)&addr, &addrLen) == 0);
        port_ = ntohs(addr.sin_port);
        REQUIRE(::listen(listenSock_, 1) == 0);
        thread_ = std::thread([this] {
            SOCKET client = ::accept(listenSock_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            try { script_(client); } catch (...) {}
            ::closesocket(client);
        });
    }
    ~MockServer() {
        ::closesocket(listenSock_);
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }

private:
    SOCKET listenSock_;
    int port_;
    Script script_;
    std::thread thread_;
};

// Read a newline-terminated frame off a socket.
std::string ReadLine(SOCKET s) {
    std::string out;
    char c;
    while (::recv(s, &c, 1, 0) == 1) {
        if (c == '\n') break;
        out.push_back(c);
    }
    return out;
}

void SendLine(SOCKET s, const std::string& line) {
    std::string framed = line + "\n";
    ::send(s, framed.data(), (int)framed.size(), 0);
}

} // namespace

TEST_CASE("LiveBackend: handshake hello/auth/auth_ok works") {
    MockServer mock([](SOCKET s) {
        // Server-side script:
        // 1. send hello
        SendLine(s, R"({"type":"hello","version":"1"})");
        // 2. read auth, validate token
        std::string authLine = ReadLine(s);
        auto auth = nlohmann::json::parse(authLine, nullptr, false);
        REQUIRE(auth.is_object());
        CHECK(auth["type"] == "auth");
        CHECK(auth["token"] == "secret123");
        // 3. respond auth_ok
        SendLine(s, R"({"type":"auth_ok"})");
        // 4. wait for one op frame, send back a synthetic List response.
        std::string opLine = ReadLine(s);
        auto op = nlohmann::json::parse(opLine, nullptr, false);
        REQUIRE(op.is_object());
        CHECK(op["type"] == "op");
        int id = op["id"].get<int>();
        nlohmann::json result = {
            {"type", "result"},
            {"id", id},
            {"code", 0},
            {"json", nlohmann::json::array({
                nlohmann::json{
                    {"asset_path", "/Game/Test/BP_Mock"},
                    {"name", "BP_Mock"},
                    {"parent_class", "/Script/Engine.Actor"},
                    {"modified_iso", "2025-01-01T00:00:00.000Z"},
                }
            })},
        };
        SendLine(s, result.dump());
    });

    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = mock.port();
    cfg.token = "secret123";
    LiveBlueprintReader reader(cfg);
    auto items = reader.ListBlueprints("/Game");
    REQUIRE(items.size() == 1);
    CHECK(items[0].AssetPath == "/Game/Test/BP_Mock");
    CHECK(items[0].ParentClass == "/Script/Engine.Actor");
}

TEST_CASE("LiveBackend: auth_fail closes connection and next op throws") {
    MockServer mock([](SOCKET s) {
        SendLine(s, R"({"type":"hello","version":"1"})");
        std::string authLine = ReadLine(s);
        // Reject — token doesn't match (test passes the wrong one).
        SendLine(s, R"({"type":"auth_fail"})");
    });

    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = mock.port();
    cfg.token = "wrong-token";
    LiveBlueprintReader reader(cfg);
    CHECK_THROWS_AS(reader.ListBlueprints("/Game"), BlueprintReaderError);
}

TEST_CASE("LiveBackend: server returns error frame surfaces as BlueprintReaderError") {
    MockServer mock([](SOCKET s) {
        SendLine(s, R"({"type":"hello","version":"1"})");
        std::string authLine = ReadLine(s);
        SendLine(s, R"({"type":"auth_ok"})");
        std::string opLine = ReadLine(s);
        // Server returns an error frame for the op.
        auto op = nlohmann::json::parse(opLine, nullptr, false);
        int id = op["id"].get<int>();
        nlohmann::json err = {
            {"type", "error"},
            {"id", id},
            {"error", "asset path is required"},
        };
        SendLine(s, err.dump());
    });

    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = mock.port();
    cfg.token = "tok";
    LiveBlueprintReader reader(cfg);
    try {
        reader.ReadBlueprint("/Game/Bad");
        FAIL("expected throw");
    } catch (const BlueprintReaderError& e) {
        std::string msg = e.what();
        CHECK(msg.find("asset path is required") != std::string::npos);
    }
}

TEST_CASE("LiveBackend: missing token in config throws at construction") {
    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 8421;
    cfg.token = "";  // empty — should fail
    CHECK_THROWS_AS(LiveBlueprintReader{cfg}, BlueprintReaderError);
}

TEST_CASE("LiveBackend: connect to closed port throws on first op") {
    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1;  // privileged port no test infra binds; refuses connect
    cfg.token = "tok";
    LiveBlueprintReader reader(cfg);  // construction is lazy — doesn't connect
    CHECK_THROWS_AS(reader.ListBlueprints("/Game"), BlueprintReaderError);
}

// ---- Issue #9: stale-port recovery via handshake refresh ------------

TEST_CASE("LiveBackend: connect failure triggers handshake re-read and retries") {
    // Simulate the editor-restart scenario: the MCP server learned a
    // stale port at startup, the editor is now listening on a different
    // port. The handshake file points at the new port. We should
    // recover automatically rather than throwing.
    MockServer mock([](SOCKET s) {
        SendLine(s, R"({"type":"hello","version":"1"})");
        ReadLine(s);
        SendLine(s, R"({"type":"auth_ok"})");
        ReadLine(s);  // op frame
        SendLine(s, nlohmann::json{
            {"type","result"}, {"id",1}, {"code",0},
            {"json", nlohmann::json::array()}
        }.dump());
    });

    // Write a handshake file pointing at the mock's actual port.
    auto tempPath = std::filesystem::temp_directory_path() /
        ("bp-reader-live-test-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&mock)) + ".json");
    nlohmann::json hs = {
        {"version", 1},
        {"host", "127.0.0.1"},
        {"port", mock.port()},
        {"token", "fresh-token"},
    };
    {
        std::ofstream f(tempPath);
        f << hs.dump();
    }

    // Configure the reader with a STALE port (1 — refused) and STALE
    // token. The reader should fail to connect, re-read the handshake,
    // pick up the mock's real port + token, retry, and succeed.
    LiveBlueprintReader::Config cfg;
    cfg.host  = "127.0.0.1";
    cfg.port  = 1;             // stale: connect refused
    cfg.token = "stale-token";
    cfg.handshakeFilePath = tempPath.string();
    LiveBlueprintReader reader(cfg);

    // If the refresh worked, ListBlueprints completes without throwing.
    CHECK_NOTHROW((void)reader.ListBlueprints("/Game"));

    std::filesystem::remove(tempPath);
}

TEST_CASE("LiveBackend: handshake refresh skipped when path empty") {
    // Backward compat: the original Config has no handshake path. A
    // connect failure should fall through to the existing error,
    // not try to refresh (we don't have a path to read).
    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1;
    cfg.token = "t";
    // cfg.handshakeFilePath intentionally empty
    LiveBlueprintReader reader(cfg);
    CHECK_THROWS_AS(reader.ListBlueprints("/Game"), BlueprintReaderError);
}

TEST_CASE("LiveBackend: handshake refresh ignored when file points at same port") {
    // If the handshake file holds the same values as cfg_ already does,
    // the refresh is a no-op — the retry would fail identically and
    // we should surface the original error rather than loop.
    LiveBlueprintReader::Config cfg;
    cfg.host  = "127.0.0.1";
    cfg.port  = 1;
    cfg.token = "t";
    auto tempPath = std::filesystem::temp_directory_path() /
        ("bp-reader-live-noop-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&cfg)) + ".json");
    nlohmann::json hs = {
        {"version", 1},
        {"host", "127.0.0.1"},
        {"port", 1},            // identical to cfg → refresh is a no-op
        {"token", "t"},
    };
    {
        std::ofstream f(tempPath);
        f << hs.dump();
    }
    cfg.handshakeFilePath = tempPath.string();
    LiveBlueprintReader reader(cfg);
    CHECK_THROWS_AS(reader.ListBlueprints("/Game"), BlueprintReaderError);
    std::filesystem::remove(tempPath);
}

TEST_CASE("LiveBackend: op frame carries the canonical -Op=List shape") {
    std::string capturedOpFrame;
    MockServer mock([&](SOCKET s) {
        SendLine(s, R"({"type":"hello","version":"1"})");
        ReadLine(s);  // auth
        SendLine(s, R"({"type":"auth_ok"})");
        capturedOpFrame = ReadLine(s);
        auto op = nlohmann::json::parse(capturedOpFrame, nullptr, false);
        int id = op["id"].get<int>();
        SendLine(s, nlohmann::json{
            {"type","result"}, {"id",id}, {"code",0},
            {"json", nlohmann::json::array()}
        }.dump());
    });
    LiveBlueprintReader::Config cfg;
    cfg.host = "127.0.0.1"; cfg.port = mock.port(); cfg.token = "t";
    LiveBlueprintReader reader(cfg);
    (void)reader.ListBlueprints("/Game/AI");

    auto frame = nlohmann::json::parse(capturedOpFrame);
    CHECK(frame["type"] == "op");
    REQUIRE(frame["args"].is_array());
    REQUIRE(frame["args"].size() == 2);
    CHECK(frame["args"][0] == "-Op=List");
    CHECK(frame["args"][1] == "-Path=/Game/AI");
}
