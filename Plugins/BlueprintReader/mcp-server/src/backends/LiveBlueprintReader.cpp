#include "backends/LiveBlueprintReader.h"

#include <fmt/core.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
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

void LiveBlueprintReader::RetypeVariable(std::string_view a, std::string_view n,
                                         const BPPinType& t) {
    std::vector<std::string> args = {
        "-Op=RetypeVariable",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
        "-TypeCategory=" + t.Category,
    };
    if (t.SubCategory)       args.push_back("-TypeSubCategory=" + *t.SubCategory);
    if (t.SubCategoryObject) args.push_back("-TypeSubCategoryObject=" + *t.SubCategoryObject);
    if (t.IsArray) args.push_back("-TypeIsArray");
    if (t.IsSet)   args.push_back("-TypeIsSet");
    if (t.IsMap)   args.push_back("-TypeIsMap");
    (void)RunOp(args);
}

void LiveBlueprintReader::SetVariableCategory(std::string_view a, std::string_view n,
                                              std::string_view category) {
    std::vector<std::string> args = {
        "-Op=SetVariableCategory",
        "-Asset=" + std::string(a),
        "-Name=" + std::string(n),
    };
    if (!category.empty()) args.push_back("-Category=" + std::string(category));
    (void)RunOp(args);
}

IBlueprintReader::WriteGeneratedSourceResult
LiveBlueprintReader::WriteGeneratedSource(std::string_view destPath,
                                          std::string_view content,
                                          bool createDirs) {
    // Same temp-file trick the commandlet uses: the wire frame format
    // would technically let us send content inline (it's JSON-shaped,
    // not line-bounded), but we keep symmetry with the commandlet so
    // both backends share the plugin op's calling convention.
    namespace fs = std::filesystem;
    fs::path tempDir = fs::temp_directory_path();
    fs::path contentTemp = tempDir /
        ("bpr-live-write-" + std::to_string(static_cast<unsigned long long>(
            std::hash<std::string>{}(std::string(destPath)))) + ".txt");
    {
        std::ofstream f(contentTemp, std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::vector<std::string> args = {
        "-Op=WriteGeneratedSource",
        "-Path=" + std::string(destPath),
        "-ContentFile=" + contentTemp.string(),
    };
    if (createDirs) args.push_back("-CreateDirs");
    auto j = RunOp(args);

    std::error_code ec;
    fs::remove(contentTemp, ec);

    WriteGeneratedSourceResult out;
    if (j.is_object()) {
        out.bytesWritten = j.value("bytes_written", std::size_t{0});
        out.path         = j.value("path", std::string{});
    }
    return out;
}

IBlueprintReader::DuplicateBlueprintResult
LiveBlueprintReader::DuplicateBlueprint(std::string_view source, std::string_view dest) {
    auto j = RunOp({
        "-Op=DuplicateBlueprint",
        "-Asset=" + std::string(source),
        "-Dest="  + std::string(dest),
    });
    DuplicateBlueprintResult out;
    out.alreadyExisted   = j.is_object() && j.value("already_existed", false);
    out.sourceAssetPath  = std::string(source);
    return out;
}

// ----- batch sentinels ---------------------------------------------------
// ----- Project + Content Browser ops ---------------------------------

IBlueprintReader::ProjectMetadata
LiveBlueprintReader::GetProjectMetadata() {
    auto j = RunOp({"-Op=GetProjectMetadata"});
    ProjectMetadata out;
    if (j.is_object()) {
        out.projectName       = j.value("project_name",       std::string{});
        out.projectPath       = j.value("project_path",       std::string{});
        out.engineAssociation = j.value("engine_association", std::string{});
        out.category          = j.value("category",           std::string{});
        out.description       = j.value("description",        std::string{});
        if (auto it = j.find("raw"); it != j.end()) out.raw = *it;
    }
    return out;
}

IBlueprintReader::SaveAllResult
LiveBlueprintReader::SaveAll(bool dirtyOnly) {
    std::vector<std::string> args = {"-Op=SaveAll"};
    if (!dirtyOnly) args.push_back("-IncludeClean");
    auto j = RunOp(args);
    SaveAllResult out;
    if (j.is_object()) {
        out.savedCount = j.value("saved_count", 0);
        if (auto it = j.find("failed_assets"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.failedAssets.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

IBlueprintReader::MoveAssetResult
LiveBlueprintReader::MoveAsset(std::string_view sourcePath,
                               std::string_view destPath) {
    auto j = RunOp({
        "-Op=MoveAsset",
        "-Asset=" + std::string(sourcePath),
        "-Dest="  + std::string(destPath),
    });
    MoveAssetResult out;
    out.sourcePath = std::string(sourcePath);
    out.destPath   = std::string(destPath);
    if (j.is_object()) {
        out.redirectorsCreated = j.value("redirectors_created", 0);
    }
    return out;
}

IBlueprintReader::DeleteAssetResult
LiveBlueprintReader::DeleteAsset(std::string_view assetPath, bool force) {
    std::vector<std::string> args = {
        "-Op=DeleteAsset",
        "-Asset=" + std::string(assetPath),
    };
    if (force) args.push_back("-Force");
    auto j = RunOp(args);
    DeleteAssetResult out;
    out.path = std::string(assetPath);
    if (j.is_object()) {
        out.deleted = j.value("deleted", false);
        if (auto it = j.find("referencing_assets"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.referencingAssets.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

IBlueprintReader::CreateFolderResult
LiveBlueprintReader::CreateFolder(std::string_view folderPath) {
    auto j = RunOp({
        "-Op=CreateFolder",
        "-Path=" + std::string(folderPath),
    });
    CreateFolderResult out;
    out.path = std::string(folderPath);
    if (j.is_object()) {
        out.alreadyExisted = j.value("already_existed", false);
    }
    return out;
}

std::vector<BPAssetSummary>
LiveBlueprintReader::ListDataTables(std::string_view path) {
    std::vector<std::string> args = {"-Op=ListDataTables"};
    if (!path.empty()) args.push_back("-Path=" + std::string(path));
    auto j = RunOp(args);
    std::vector<BPAssetSummary> out;
    if (j.is_array()) {
        for (const auto& v : j) {
            BPAssetSummary s;
            from_json(v, s);
            out.push_back(std::move(s));
        }
    }
    return out;
}

IBlueprintReader::DataTableInfo
LiveBlueprintReader::ReadDataTable(std::string_view assetPath) {
    auto j = RunOp({
        "-Op=ReadDataTable",
        "-Asset=" + std::string(assetPath),
    });
    DataTableInfo out;
    out.assetPath = std::string(assetPath);
    if (j.is_object()) {
        out.rowStruct = j.value("row_struct", std::string{});
        if (auto it = j.find("columns"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.columns.push_back(v.get<std::string>());
            }
        }
        if (auto it = j.find("rows"); it != j.end() && it->is_array()) {
            out.rows = *it;
        }
    }
    return out;
}

IBlueprintReader::AddDataRowResult
LiveBlueprintReader::AddDataRow(std::string_view assetPath,
                                std::string_view rowName,
                                const nlohmann::json& values,
                                bool overwrite) {
    namespace fs = std::filesystem;
    fs::path tempDir = fs::temp_directory_path();
    fs::path valuesTemp = tempDir /
        ("bpr-live-add-row-" + std::to_string(static_cast<unsigned long long>(
            std::hash<std::string>{}(std::string(assetPath) + ":" +
                                      std::string(rowName)))) + ".json");
    {
        std::ofstream f(valuesTemp);
        f << values.dump();
    }
    std::vector<std::string> args = {
        "-Op=AddDataRow",
        "-Asset=" + std::string(assetPath),
        "-Row="   + std::string(rowName),
        "-ValuesFile=" + valuesTemp.string(),
    };
    if (overwrite) args.push_back("-Overwrite");
    auto j = RunOp(args);
    std::error_code ec;
    fs::remove(valuesTemp, ec);

    AddDataRowResult out;
    out.assetPath = std::string(assetPath);
    out.rowName   = std::string(rowName);
    if (j.is_object()) {
        out.alreadyExisted = j.value("already_existed", false);
        out.created        = j.value("created", false);
    }
    return out;
}

IBlueprintReader::SetDataRowValueResult
LiveBlueprintReader::SetDataRowValue(std::string_view assetPath,
                                     std::string_view rowName,
                                     std::string_view fieldName,
                                     std::string_view value) {
    auto j = RunOp({
        "-Op=SetDataRowValue",
        "-Asset=" + std::string(assetPath),
        "-Row="   + std::string(rowName),
        "-Field=" + std::string(fieldName),
        "-Value=" + std::string(value),
    });
    SetDataRowValueResult out;
    out.assetPath = std::string(assetPath);
    out.rowName   = std::string(rowName);
    out.fieldName = std::string(fieldName);
    if (j.is_object()) {
        out.oldValue = j.value("old_value", std::string{});
        out.newValue = j.value("new_value", std::string{});
    }
    return out;
}

// ----- Component (SCS) authoring -----------------------------------------

IBlueprintReader::AddComponentResult
LiveBlueprintReader::AddComponent(std::string_view assetPath,
                                  std::string_view name,
                                  std::string_view componentClass,
                                  std::string_view parentName,
                                  std::string_view socket) {
    std::vector<std::string> args = {
        "-Op=AddComponent",
        "-Asset=" + std::string(assetPath),
        "-Name="  + std::string(name),
        "-Class=" + std::string(componentClass),
    };
    if (!parentName.empty()) args.push_back("-Parent=" + std::string(parentName));
    if (!socket.empty())     args.push_back("-Socket=" + std::string(socket));
    auto j = RunOp(args);
    AddComponentResult out;
    out.assetPath      = std::string(assetPath);
    out.name           = std::string(name);
    out.componentClass = std::string(componentClass);
    if (j.is_object()) {
        out.alreadyExisted = j.value("already_existed", false);
        out.created        = j.value("created", false);
    }
    return out;
}

IBlueprintReader::RemoveComponentResult
LiveBlueprintReader::RemoveComponent(std::string_view assetPath,
                                     std::string_view name) {
    auto j = RunOp({"-Op=RemoveComponent",
                    "-Asset=" + std::string(assetPath),
                    "-Name="  + std::string(name)});
    RemoveComponentResult out;
    out.assetPath = std::string(assetPath);
    out.name      = std::string(name);
    if (j.is_object()) out.removed = j.value("removed", false);
    return out;
}

IBlueprintReader::AttachComponentResult
LiveBlueprintReader::AttachComponent(std::string_view assetPath,
                                     std::string_view name,
                                     std::string_view newParentName,
                                     std::string_view socket) {
    std::vector<std::string> args = {
        "-Op=AttachComponent",
        "-Asset=" + std::string(assetPath),
        "-Name="  + std::string(name),
    };
    if (!newParentName.empty()) args.push_back("-NewParent=" + std::string(newParentName));
    if (!socket.empty())        args.push_back("-Socket="    + std::string(socket));
    auto j = RunOp(args);
    AttachComponentResult out;
    out.assetPath     = std::string(assetPath);
    out.name          = std::string(name);
    out.newParentName = std::string(newParentName);
    out.socket        = std::string(socket);
    if (j.is_object()) out.reparented = j.value("reparented", false);
    return out;
}

IBlueprintReader::SetComponentPropertyResult
LiveBlueprintReader::SetComponentProperty(std::string_view assetPath,
                                          std::string_view componentName,
                                          std::string_view propertyName,
                                          std::string_view value) {
    auto j = RunOp({"-Op=SetComponentProperty",
                    "-Asset="     + std::string(assetPath),
                    "-Component=" + std::string(componentName),
                    "-Property="  + std::string(propertyName),
                    "-Value="     + std::string(value)});
    SetComponentPropertyResult out;
    out.assetPath     = std::string(assetPath);
    out.componentName = std::string(componentName);
    out.propertyName  = std::string(propertyName);
    if (j.is_object()) {
        out.oldValue = j.value("old_value", std::string{});
        out.newValue = j.value("new_value", std::string{});
    }
    return out;
}

// ----- Live editor ops ---------------------------------------------------

IBlueprintReader::ConsoleCommandResult
LiveBlueprintReader::ConsoleCommand(std::string_view command) {
    auto j = RunOp({"-Op=ConsoleCommand",
                    "-Command=" + std::string(command)});
    ConsoleCommandResult out;
    if (j.is_object()) out.output = j.value("output", std::string{});
    return out;
}

IBlueprintReader::CVarValue
LiveBlueprintReader::GetCVar(std::string_view name) {
    auto j = RunOp({"-Op=GetCVar", "-Name=" + std::string(name)});
    CVarValue out;
    out.name = std::string(name);
    if (j.is_object()) {
        out.value  = j.value("value",  std::string{});
        out.help   = j.value("help",   std::string{});
        out.exists = j.value("exists", false);
    }
    return out;
}

IBlueprintReader::CVarValue
LiveBlueprintReader::SetCVar(std::string_view name, std::string_view value) {
    auto j = RunOp({"-Op=SetCVar",
                    "-Name="  + std::string(name),
                    "-Value=" + std::string(value)});
    CVarValue out;
    out.name = std::string(name);
    if (j.is_object()) {
        out.value  = j.value("value",  std::string{});
        out.help   = j.value("help",   std::string{});
        out.exists = j.value("exists", false);
    }
    return out;
}

IBlueprintReader::PieResult
LiveBlueprintReader::PieStart(std::string_view mode) {
    std::vector<std::string> args = {"-Op=PieStart"};
    if (!mode.empty()) args.push_back("-Mode=" + std::string(mode));
    auto j = RunOp(args);
    PieResult out;
    if (j.is_object()) {
        out.started = j.value("started", false);
        out.mode    = j.value("mode",    std::string{});
    }
    return out;
}

IBlueprintReader::PieResult LiveBlueprintReader::PieStop() {
    auto j = RunOp({"-Op=PieStop"});
    PieResult out;
    if (j.is_object()) out.stopped = j.value("stopped", false);
    return out;
}

IBlueprintReader::LiveCodingResult
LiveBlueprintReader::LiveCodingCompile() {
    auto j = RunOp({"-Op=LiveCodingCompile"});
    LiveCodingResult out;
    if (j.is_object()) {
        out.queued  = j.value("queued",  false);
        out.message = j.value("message", std::string{});
    }
    return out;
}

IBlueprintReader::SelectionResult
LiveBlueprintReader::GetSelectedActors() {
    auto j = RunOp({"-Op=GetSelectedActors"});
    SelectionResult out;
    if (j.is_object()) {
        if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.actorNames.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

IBlueprintReader::SelectionResult
LiveBlueprintReader::SetSelection(const std::vector<std::string>& actorNames,
                                  bool replace) {
    std::string joined;
    for (std::size_t i = 0; i < actorNames.size(); ++i) {
        if (i) joined += ",";
        joined += actorNames[i];
    }
    std::vector<std::string> args = {"-Op=SetSelection", "-Names=" + joined};
    if (!replace) args.push_back("-Add");
    auto j = RunOp(args);
    SelectionResult out;
    if (j.is_object()) {
        if (auto it = j.find("actor_names"); it != j.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string()) out.actorNames.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

IBlueprintReader::SpawnActorResult
LiveBlueprintReader::SpawnActor(std::string_view classPath,
    double locX, double locY, double locZ,
    double rotPitch, double rotYaw, double rotRoll,
    double scaleX, double scaleY, double scaleZ) {
    auto j = RunOp({
        "-Op=SpawnActor",
        "-Class=" + std::string(classPath),
        "-LocX=" + std::to_string(locX),
        "-LocY=" + std::to_string(locY),
        "-LocZ=" + std::to_string(locZ),
        "-RotPitch=" + std::to_string(rotPitch),
        "-RotYaw="   + std::to_string(rotYaw),
        "-RotRoll="  + std::to_string(rotRoll),
        "-ScaleX=" + std::to_string(scaleX),
        "-ScaleY=" + std::to_string(scaleY),
        "-ScaleZ=" + std::to_string(scaleZ),
    });
    SpawnActorResult out;
    if (j.is_object()) {
        out.actorName  = j.value("actor_name",  std::string{});
        out.actorLabel = j.value("actor_label", std::string{});
    }
    return out;
}

void LiveBlueprintReader::SetActorTransform(std::string_view actorName,
    double locX, double locY, double locZ,
    double rotPitch, double rotYaw, double rotRoll,
    double scaleX, double scaleY, double scaleZ) {
    (void)RunOp({
        "-Op=SetActorTransform",
        "-Name=" + std::string(actorName),
        "-LocX=" + std::to_string(locX),
        "-LocY=" + std::to_string(locY),
        "-LocZ=" + std::to_string(locZ),
        "-RotPitch=" + std::to_string(rotPitch),
        "-RotYaw="   + std::to_string(rotYaw),
        "-RotRoll="  + std::to_string(rotRoll),
        "-ScaleX=" + std::to_string(scaleX),
        "-ScaleY=" + std::to_string(scaleY),
        "-ScaleZ=" + std::to_string(scaleZ),
    });
}

IBlueprintReader::DeleteActorResult
LiveBlueprintReader::DeleteActor(std::string_view actorName) {
    auto j = RunOp({"-Op=DeleteActor", "-Name=" + std::string(actorName)});
    DeleteActorResult out;
    if (j.is_object()) out.deleted = j.value("deleted", false);
    return out;
}

IBlueprintReader::OutputLogResult
LiveBlueprintReader::ReadOutputLog(int limit, std::string_view minSeverity) {
    std::vector<std::string> args = {"-Op=ReadOutputLog",
                                      "-Limit=" + std::to_string(limit)};
    if (!minSeverity.empty()) {
        args.push_back("-MinSeverity=" + std::string(minSeverity));
    }
    auto j = RunOp(args);
    OutputLogResult out;
    if (j.is_object()) {
        if (auto it = j.find("entries"); it != j.end() && it->is_array()) {
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                LogEntry entry;
                entry.severity  = e.value("severity",  std::string{});
                entry.category  = e.value("category",  std::string{});
                entry.message   = e.value("message",   std::string{});
                entry.timestamp = e.value("timestamp", std::string{});
                out.entries.push_back(std::move(entry));
            }
        }
    }
    return out;
}

IBlueprintReader::AutomationRunResult
LiveBlueprintReader::RunAutomationTests(std::string_view pattern) {
    std::vector<std::string> args = {"-Op=RunAutomationTests"};
    if (!pattern.empty()) args.push_back("-Pattern=" + std::string(pattern));
    auto j = RunOp(args);
    AutomationRunResult out;
    if (j.is_object()) {
        out.started = j.value("started", false);
        out.message = j.value("message", std::string{});
    }
    return out;
}

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
