#include "backends/LiveBlueprintReader.h"

#include <fmt/core.h>

#include <stdexcept>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    using SocketType = SOCKET;
    static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
    using SocketType = int;
    static constexpr SocketType kInvalidSocket = -1;
#endif

namespace bpr::backends {

namespace {

// On Windows, WSAStartup is required before any socket call. We refcount
// across LiveBlueprintReader instances so the startup/cleanup pair is
// balanced even if multiple readers exist (rare, but cheap to be right).
#if defined(_WIN32)
struct WsaScope {
    WsaScope() {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            throw BlueprintReaderError("WSAStartup failed");
        }
    }
    ~WsaScope() { WSACleanup(); }
};
WsaScope& Wsa() {
    static WsaScope s;
    return s;
}
#endif

// Send all bytes or throw. Wraps the loop pattern around partial sends.
void SendAll(SocketType s, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, data + sent,
                       static_cast<int>(std::min<std::size_t>(len - sent, 1 << 20)),
                       0);
        if (n <= 0) {
            throw BlueprintReaderError("LiveBlueprintReader: socket write failed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

// Read a newline-terminated frame. Buffers extra bytes (the next frame
// may have arrived in the same recv) into `pending` for later reads.
std::string RecvLine(SocketType s, std::string& pending) {
    while (true) {
        auto nl = pending.find('\n');
        if (nl != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            // Trim trailing \r if present (defensive against CRLF servers).
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            return line;
        }
        char buf[4096];
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            throw BlueprintReaderError(
                "LiveBlueprintReader: connection closed before frame complete");
        }
        pending.append(buf, static_cast<std::size_t>(n));
    }
}

// Per-instance read buffer. Wrap in a struct so RecvLine's `pending` ref
// has somewhere persistent to live across calls without leaking buffer
// state into the public header.
struct PendingBuf {
    std::string b;
};
PendingBuf& BufFor(SocketType s) {
    // We have one socket per reader; this map is ridiculous overkill but
    // keeps the impl simple. Real impl would carry the buffer as a class
    // member — refactor if multiple sockets per reader ever happens.
    thread_local std::map<SocketType, PendingBuf> bufs;
    return bufs[s];
}

} // namespace

LiveBlueprintReader::LiveBlueprintReader(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.port <= 0) {
        throw BlueprintReaderError(
            "LiveBlueprintReader: BP_READER_LIVE_PORT must be set to a valid port");
    }
    if (cfg_.token.empty()) {
        throw BlueprintReaderError(
            "LiveBlueprintReader: BP_READER_LIVE_TOKEN must be set "
            "(also required on the editor side; the values must match)");
    }
#if defined(_WIN32)
    (void)Wsa();  // first construction triggers WSAStartup
#endif
    // Connect lazily on first RunOp — keeps construction cheap and
    // non-throwing when the editor isn't running yet.
}

LiveBlueprintReader::~LiveBlueprintReader() {
    Disconnect();
}

void LiveBlueprintReader::Disconnect() {
    if (socket_ != static_cast<intptr_t>(kInvalidSocket)) {
#if defined(_WIN32)
        closesocket(static_cast<SocketType>(socket_));
#else
        close(static_cast<SocketType>(socket_));
#endif
        socket_ = static_cast<intptr_t>(kInvalidSocket);
    }
    handshakeOk_ = false;
}

void LiveBlueprintReader::EnsureConnected() {
    if (handshakeOk_) return;

    SocketType s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        throw BlueprintReaderError("LiveBlueprintReader: socket() failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
    inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        throw BlueprintReaderError(fmt::format(
            "LiveBlueprintReader: connect to {}:{} failed — is the editor "
            "running with BP_READER_LIVE_PORT set?",
            cfg_.host, cfg_.port));
    }
    socket_ = static_cast<intptr_t>(s);

    // Handshake: read hello, send auth, read auth_ok.
    auto& buf = BufFor(s).b;
    buf.clear();
    std::string hello = RecvLine(s, buf);
    auto helloJson = nlohmann::json::parse(hello, nullptr, /*allow_exceptions=*/false);
    if (!helloJson.is_object() || helloJson.value("type", "") != "hello") {
        Disconnect();
        throw BlueprintReaderError(fmt::format(
            "LiveBlueprintReader: expected hello frame, got: {}", hello));
    }

    nlohmann::json authMsg = {
        {"type", "auth"},
        {"token", cfg_.token},
    };
    std::string authLine = authMsg.dump() + "\n";
    SendAll(s, authLine.data(), authLine.size());

    std::string authResp = RecvLine(s, buf);
    auto authJson = nlohmann::json::parse(authResp, nullptr, false);
    std::string respType = authJson.is_object() ? authJson.value("type", "") : "";
    if (respType != "auth_ok") {
        Disconnect();
        throw BlueprintReaderError(fmt::format(
            "LiveBlueprintReader: auth failed (server response: {}). "
            "Check that BP_READER_LIVE_TOKEN is identical in the editor's "
            "process env and the MCP server's process env.", authResp));
    }
    handshakeOk_ = true;
}

nlohmann::json LiveBlueprintReader::RunOp(const std::vector<std::string>& args) {
    std::lock_guard lock(mu_);
    EnsureConnected();
    SocketType s = static_cast<SocketType>(socket_);

    int id = nextRequestId_++;
    nlohmann::json frame = {
        {"type", "op"},
        {"id", id},
        {"args", args},
    };
    std::string line = frame.dump() + "\n";
    try {
        SendAll(s, line.data(), line.size());
    } catch (...) {
        Disconnect();  // socket is broken; force re-handshake on next call
        throw;
    }

    auto& buf = BufFor(s).b;
    std::string response;
    try {
        response = RecvLine(s, buf);
    } catch (...) {
        Disconnect();
        throw;
    }
    auto j = nlohmann::json::parse(response, nullptr, false);
    if (!j.is_object()) {
        Disconnect();
        throw BlueprintReaderError(
            "LiveBlueprintReader: server response wasn't a JSON object");
    }
    if (j.value("type", "") == "error") {
        throw BlueprintReaderError(fmt::format(
            "live op '{}': {}",
            args.empty() ? "<unknown>" : args[0],
            j.value("error", "unspecified error")));
    }
    int code = j.value("code", -1);
    if (code != 0) {
        throw BlueprintReaderError(fmt::format(
            "live op '{}' returned code={} (response: {})",
            args.empty() ? "<unknown>" : args[0], code, response));
    }
    auto jit = j.find("json");
    if (jit == j.end()) return nlohmann::json::object();
    return *jit;
}

// ----- read tools --------------------------------------------------------
// Same op-args shape as CommandletBlueprintReader, just routed over TCP.
// Type conversion uses nlohmann::json's automatic deserialization adapters
// in BlueprintReaderTypes.h.

std::vector<BPAssetSummary>
LiveBlueprintReader::ListBlueprints(std::string_view path) {
    std::vector<std::string> args = {"-Op=List"};
    if (!path.empty()) args.push_back("-Path=" + std::string(path));
    return RunOp(args).get<std::vector<BPAssetSummary>>();
}

BPMetadata LiveBlueprintReader::ReadBlueprint(std::string_view assetPath) {
    return RunOp({"-Op=Read", "-Asset=" + std::string(assetPath)}).get<BPMetadata>();
}

BPGraph LiveBlueprintReader::GetGraph(std::string_view assetPath, std::string_view graphName) {
    return RunOp({
        "-Op=Graph",
        "-Asset=" + std::string(assetPath),
        "-Graph=" + std::string(graphName),
    }).get<BPGraph>();
}

BPFunction LiveBlueprintReader::GetFunction(std::string_view assetPath, std::string_view fnName) {
    return RunOp({
        "-Op=Function",
        "-Asset=" + std::string(assetPath),
        "-Function=" + std::string(fnName),
    }).get<BPFunction>();
}

std::vector<BPVariable>
LiveBlueprintReader::ListVariables(std::string_view assetPath) {
    return RunOp({
        "-Op=Variables",
        "-Asset=" + std::string(assetPath),
    }).get<std::vector<BPVariable>>();
}

std::vector<BPComponent>
LiveBlueprintReader::GetComponents(std::string_view assetPath) {
    return RunOp({
        "-Op=Components",
        "-Asset=" + std::string(assetPath),
    }).get<std::vector<BPComponent>>();
}

std::vector<BPNode>
LiveBlueprintReader::FindNode(std::string_view assetPath, std::string_view query,
                              std::string_view kind) {
    std::vector<std::string> args = {
        "-Op=Find",
        "-Asset=" + std::string(assetPath),
        "-Query=" + std::string(query),
    };
    if (!kind.empty()) args.push_back("-Kind=" + std::string(kind));
    return RunOp(args).get<std::vector<BPNode>>();
}

// ----- write tools -------------------------------------------------------
// Same op-args the commandlet daemon already accepts. Editor-side
// dispatch happens on the game thread inside RunOneOp.

void LiveBlueprintReader::AddVariable(std::string_view a, std::string_view n,
                                      const BPPinType& t, std::string_view dv,
                                      std::string_view cat, bool repl, bool edit) {
    std::vector<std::string> args = {
        "-Op=AddVariable",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
        "-TypeCategory=" + t.Category,
    };
    if (t.SubCategory)       args.push_back("-TypeSubCategory=" + *t.SubCategory);
    if (t.SubCategoryObject) args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
    if (t.IsArray) args.push_back("-TypeIsArray");
    if (t.IsSet)   args.push_back("-TypeIsSet");
    if (t.IsMap)   args.push_back("-TypeIsMap");
    if (!dv.empty())  args.push_back("-Default=" + std::string(dv));
    if (!cat.empty()) args.push_back("-Category=" + std::string(cat));
    if (repl) args.push_back("-Replicated");
    if (edit) args.push_back("-Editable");
    (void)RunOp(args);
}

void LiveBlueprintReader::SetNodePosition(std::string_view a, std::string_view g,
                                          std::string_view n, int x, int y) {
    (void)RunOp({
        "-Op=SetNodePosition",
        "-Asset=" + std::string(a),
        "-Graph=" + std::string(g),
        "-Node=" + std::string(n),
        "-X=" + std::to_string(x),
        "-Y=" + std::to_string(y),
    });
}

void LiveBlueprintReader::DeleteNode(std::string_view a, std::string_view g,
                                     std::string_view n) {
    (void)RunOp({
        "-Op=DeleteNode",
        "-Asset=" + std::string(a),
        "-Graph=" + std::string(g),
        "-Node=" + std::string(n),
    });
}

std::string LiveBlueprintReader::AddNode(std::string_view a, std::string_view g,
                                         std::string_view k, int x, int y,
                                         const std::map<std::string, std::string, std::less<>>& extras) {
    std::vector<std::string> args = {
        "-Op=AddNode",
        "-Asset=" + std::string(a),
        "-Graph=" + std::string(g),
        "-Kind=" + std::string(k),
        "-X=" + std::to_string(x),
        "-Y=" + std::to_string(y),
    };
    for (const auto& [key, val] : extras) {
        args.push_back("-" + key + "=" + val);
    }
    auto j = RunOp(args);
    return j.value("node_id", std::string{});
}

void LiveBlueprintReader::WirePins(std::string_view a, std::string_view g,
                                   std::string_view fn, std::string_view fp,
                                   std::string_view tn, std::string_view tp) {
    (void)RunOp({
        "-Op=WirePins",
        "-Asset=" + std::string(a),
        "-Graph=" + std::string(g),
        "-FromNode=" + std::string(fn),
        "-FromPin=" + std::string(fp),
        "-ToNode=" + std::string(tn),
        "-ToPin=" + std::string(tp),
    });
}

void LiveBlueprintReader::DeleteVariable(std::string_view a, std::string_view n) {
    (void)RunOp({
        "-Op=DeleteVariable",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
    });
}

void LiveBlueprintReader::RenameVariable(std::string_view a, std::string_view oldN,
                                         std::string_view newN) {
    (void)RunOp({
        "-Op=RenameVariable",
        "-Asset=" + std::string(a),
        "-OldName=" + std::string(oldN),
        "-NewName=" + std::string(newN),
    });
}

IBlueprintReader::AddFunctionResult
LiveBlueprintReader::AddFunction(std::string_view a, std::string_view n) {
    auto j = RunOp({
        "-Op=AddFunction",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
    });
    AddFunctionResult out;
    out.functionName = j.value("function_name", std::string(n));
    out.entryNodeId  = j.value("entry_node_id", std::string{});
    return out;
}

void LiveBlueprintReader::AddFunctionInput(std::string_view a, std::string_view fn,
                                           std::string_view p, const BPPinType& t) {
    std::vector<std::string> args = {
        "-Op=AddFunctionInput",
        "-Asset=" + std::string(a),
        "-Function=" + std::string(fn),
        "-Param=" + std::string(p),
        "-TypeCategory=" + t.Category,
    };
    if (t.SubCategory)       args.push_back("-TypeSubCategory=" + *t.SubCategory);
    if (t.SubCategoryObject) args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
    if (t.IsArray) args.push_back("-TypeIsArray");
    if (t.IsSet)   args.push_back("-TypeIsSet");
    if (t.IsMap)   args.push_back("-TypeIsMap");
    (void)RunOp(args);
}

void LiveBlueprintReader::AddFunctionOutput(std::string_view a, std::string_view fn,
                                            std::string_view p, const BPPinType& t) {
    std::vector<std::string> args = {
        "-Op=AddFunctionOutput",
        "-Asset=" + std::string(a),
        "-Function=" + std::string(fn),
        "-Param=" + std::string(p),
        "-TypeCategory=" + t.Category,
    };
    if (t.SubCategory)       args.push_back("-TypeSubCategory=" + *t.SubCategory);
    if (t.SubCategoryObject) args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
    if (t.IsArray) args.push_back("-TypeIsArray");
    if (t.IsSet)   args.push_back("-TypeIsSet");
    if (t.IsMap)   args.push_back("-TypeIsMap");
    (void)RunOp(args);
}

void LiveBlueprintReader::DeleteFunction(std::string_view a, std::string_view n) {
    (void)RunOp({
        "-Op=DeleteFunction",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
    });
}

void LiveBlueprintReader::SetVariableDefault(std::string_view a, std::string_view n,
                                             std::string_view d) {
    std::vector<std::string> args = {
        "-Op=SetVariableDefault",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
    };
    if (!d.empty()) args.push_back("-Default=" + std::string(d));
    (void)RunOp(args);
}

IBlueprintReader::CreateBlueprintResult
LiveBlueprintReader::CreateBlueprint(std::string_view a, std::string_view p) {
    auto j = RunOp({
        "-Op=CreateBlueprint",
        "-Asset=" + std::string(a),
        "-ParentClass=" + std::string(p),
    });
    CreateBlueprintResult out;
    out.alreadyExisted = j.value("already_existed", false);
    out.parentClass    = j.value("parent_class",    std::string{});
    return out;
}

void LiveBlueprintReader::SetPinDefault(std::string_view a, std::string_view g,
                                        std::string_view n, std::string_view pin,
                                        std::string_view v) {
    (void)RunOp({
        "-Op=SetPinDefault",
        "-Asset=" + std::string(a),
        "-Graph=" + std::string(g),
        "-Node=" + std::string(n),
        "-Pin=" + std::string(pin),
        "-Value=" + std::string(v),
    });
}

// ----- batch sentinels ---------------------------------------------------
void LiveBlueprintReader::BeginBatch() {
    (void)RunOp({"-Op=BeginBatch"});
}

nlohmann::json LiveBlueprintReader::EndBatch(bool skipCompile) {
    std::vector<std::string> args = {"-Op=EndBatch"};
    if (skipCompile) args.push_back("-Skip");
    return RunOp(args);
}

nlohmann::json LiveBlueprintReader::ShutdownDaemon() {
    // Live mode has no daemon to shut down — the editor runs
    // independently of the MCP server's lifetime. The right "shutdown"
    // semantics here would be "close the socket connection", which
    // happens automatically in the destructor anyway. Return a neutral
    // ack so callers don't see this as an error.
    return nlohmann::json{
        {"ok", true},
        {"was_running", false},
        {"hint", "Live backend has no daemon process; close the editor "
                 "to release project locks."},
    };
}

} // namespace bpr::backends
