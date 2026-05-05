#include "jsonrpc/Server.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <fmt/core.h>

namespace bpr::jsonrpc {

namespace {

std::string TrimAscii(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool ReadHeaderLine(std::istream& in, std::string& out) {
    out.clear();
    while (true) {
        int c = in.get();
        if (c == std::char_traits<char>::eof()) {
            return !out.empty(); // truncated header at EOF
        }
        if (c == '\r') {
            int n = in.get();
            if (n != '\n') {
                throw std::runtime_error("malformed header: CR not followed by LF");
            }
            return true;
        }
        if (c == '\n') {
            // Tolerate bare-LF line endings.
            return true;
        }
        out.push_back(static_cast<char>(c));
    }
}

} // namespace

std::optional<std::string> ReadFrame(std::istream& in) {
    std::size_t contentLength = 0;
    bool sawContentLength = false;

    while (true) {
        std::string line;
        if (!ReadHeaderLine(in, line)) {
            // Clean EOF before any header.
            return std::nullopt;
        }
        if (line.empty()) {
            // End of headers.
            break;
        }

        // Parse "Header-Name: value".
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error(fmt::format("malformed header line: '{}'", line));
        }
        std::string name = TrimAscii(line.substr(0, colon));
        std::string value = TrimAscii(line.substr(colon + 1));

        // Header names are case-insensitive.
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (name == "content-length") {
            try {
                contentLength = static_cast<std::size_t>(std::stoull(value));
                sawContentLength = true;
            } catch (...) {
                throw std::runtime_error(fmt::format("invalid Content-Length: '{}'", value));
            }
        }
        // Other headers (e.g. Content-Type) are accepted and ignored.
    }

    if (!sawContentLength) {
        throw std::runtime_error("missing Content-Length header");
    }

    std::string body(contentLength, '\0');
    in.read(body.data(), static_cast<std::streamsize>(contentLength));
    auto got = in.gcount();
    if (static_cast<std::size_t>(got) != contentLength) {
        throw std::runtime_error(fmt::format(
            "short read on body: expected {}, got {}", contentLength, got));
    }
    return body;
}

void WriteFrame(std::ostream& out, const nlohmann::json& body) {
    std::string serialized = body.dump();
    out << "Content-Length: " << serialized.size() << "\r\n\r\n" << serialized;
    out.flush();
}

void Server::Register(std::string method, Handler handler) {
    handlers_[std::move(method)] = std::move(handler);
}

nlohmann::json MakeResultEnvelope(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json env = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
    return env;
}

nlohmann::json MakeErrorEnvelope(const nlohmann::json& id, const Error& err) {
    nlohmann::json e = {
        {"code", err.code},
        {"message", err.message},
    };
    if (err.data.has_value()) {
        e["data"] = *err.data;
    }
    nlohmann::json env = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", std::move(e)},
    };
    return env;
}

std::optional<nlohmann::json> Server::Dispatch(const nlohmann::json& body) {
    // Validate envelope.
    if (!body.is_object()) {
        return MakeErrorEnvelope(nullptr,
            Error{static_cast<int>(ErrorCode::InvalidRequest),
                  "request must be a JSON object", std::nullopt});
    }
    auto idIt = body.find("id");
    nlohmann::json id = (idIt != body.end()) ? *idIt : nlohmann::json(nullptr);
    const bool isNotification = (idIt == body.end());

    auto jsonrpcIt = body.find("jsonrpc");
    if (jsonrpcIt == body.end() || !jsonrpcIt->is_string() || jsonrpcIt->get<std::string>() != "2.0") {
        if (isNotification) return std::nullopt;
        return MakeErrorEnvelope(id,
            Error{static_cast<int>(ErrorCode::InvalidRequest),
                  R"(missing or unsupported "jsonrpc" version)", std::nullopt});
    }

    auto methodIt = body.find("method");
    if (methodIt == body.end() || !methodIt->is_string()) {
        if (isNotification) return std::nullopt;
        return MakeErrorEnvelope(id,
            Error{static_cast<int>(ErrorCode::InvalidRequest),
                  R"(missing or non-string "method")", std::nullopt});
    }
    std::string method = methodIt->get<std::string>();

    nlohmann::json params = nlohmann::json::object();
    auto paramsIt = body.find("params");
    if (paramsIt != body.end()) {
        params = *paramsIt;
    }

    auto h = handlers_.find(method);
    if (h == handlers_.end()) {
        if (isNotification) return std::nullopt;
        return MakeErrorEnvelope(id,
            Error{static_cast<int>(ErrorCode::MethodNotFound),
                  fmt::format("method not found: {}", method), std::nullopt});
    }

    Response resp;
    try {
        resp = h->second(params);
    } catch (const std::exception& e) {
        if (isNotification) return std::nullopt;
        return MakeErrorEnvelope(id,
            Error{static_cast<int>(ErrorCode::InternalError),
                  fmt::format("handler threw: {}", e.what()), std::nullopt});
    }

    if (isNotification) {
        return std::nullopt;
    }

    if (resp.error.has_value()) {
        return MakeErrorEnvelope(id, *resp.error);
    }
    return MakeResultEnvelope(id, resp.result.value_or(nlohmann::json::object()));
}

void Server::Run(std::istream& in, std::ostream& out, std::ostream& log) {
    while (true) {
        std::optional<std::string> frame;
        try {
            frame = ReadFrame(in);
        } catch (const std::exception& e) {
            log << "[bp-reader-mcp] frame error: " << e.what() << "\n";
            // Without framing we can't recover the stream; bail.
            return;
        }
        if (!frame) {
            // Clean EOF.
            return;
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(*frame);
        } catch (const std::exception& e) {
            // Per spec: parse errors get a response with id=null.
            auto env = MakeErrorEnvelope(nullptr,
                Error{static_cast<int>(ErrorCode::ParseError),
                      fmt::format("parse error: {}", e.what()), std::nullopt});
            WriteFrame(out, env);
            continue;
        }

        // Batched requests: array of bodies. Per JSON-RPC 2.0 we respond with
        // an array of responses. MCP doesn't use batches in practice but it's
        // cheap to support.
        if (body.is_array()) {
            if (body.empty()) {
                auto env = MakeErrorEnvelope(nullptr,
                    Error{static_cast<int>(ErrorCode::InvalidRequest),
                          "empty batch", std::nullopt});
                WriteFrame(out, env);
                continue;
            }
            nlohmann::json batchOut = nlohmann::json::array();
            for (const auto& sub : body) {
                if (auto r = Dispatch(sub)) {
                    batchOut.push_back(std::move(*r));
                }
            }
            if (!batchOut.empty()) {
                WriteFrame(out, batchOut);
            }
            continue;
        }

        if (auto r = Dispatch(body)) {
            WriteFrame(out, *r);
        }
    }
}

} // namespace bpr::jsonrpc
