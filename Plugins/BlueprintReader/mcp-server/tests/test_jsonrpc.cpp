// Tests for the JSON-RPC framing + dispatch layer.

#include <doctest/doctest.h>

#include "jsonrpc/Server.h"

#include <sstream>

using namespace bpr::jsonrpc;
using nlohmann::json;

namespace {

std::string FrameNL(const json& body) {
    std::ostringstream os;
    WriteFrame(os, body, FrameFormat::NewlineDelimited);
    return os.str();
}
std::string FrameCL(const json& body) {
    std::ostringstream os;
    WriteFrame(os, body, FrameFormat::ContentLength);
    return os.str();
}

} // namespace

TEST_CASE("WriteFrame newline-delimited emits one line + LF") {
    json body = {{"hello", "world"}};
    auto out = FrameNL(body);
    REQUIRE(!out.empty());
    REQUIRE(out.back() == '\n');
    auto parsed = json::parse(out.substr(0, out.size() - 1));
    CHECK(parsed["hello"] == "world");
}

TEST_CASE("WriteFrame Content-Length emits header and body") {
    json body = {{"hello", "world"}};
    auto out = FrameCL(body);
    auto headerEnd = out.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    auto headers = out.substr(0, headerEnd);
    auto payload = out.substr(headerEnd + 4);
    CHECK(headers.find("Content-Length: ") == 0);
    auto parsed = json::parse(payload);
    CHECK(parsed["hello"] == "world");
}

TEST_CASE("ReadFrame auto-detects newline-delimited and round-trips") {
    json body = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
    auto framed = FrameNL(body);
    std::istringstream is(framed);
    FrameFormat fmt = FrameFormat::ContentLength;  // wrong default, should get overwritten
    auto raw = ReadFrame(is, &fmt);
    REQUIRE(raw.has_value());
    CHECK(fmt == FrameFormat::NewlineDelimited);
    CHECK(json::parse(*raw) == body);
    CHECK_FALSE(ReadFrame(is).has_value());
}

TEST_CASE("ReadFrame auto-detects Content-Length and round-trips") {
    json body = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
    auto framed = FrameCL(body);
    std::istringstream is(framed);
    FrameFormat fmt = FrameFormat::NewlineDelimited;  // wrong default, should get overwritten
    auto raw = ReadFrame(is, &fmt);
    REQUIRE(raw.has_value());
    CHECK(fmt == FrameFormat::ContentLength);
    CHECK(json::parse(*raw) == body);
    CHECK_FALSE(ReadFrame(is).has_value());
}

TEST_CASE("ReadFrame errors on Content-Length-style frame missing the header") {
    // Starts with non-JSON, non-Content-Length character → parsed as headers,
    // hits "missing Content-Length" since "Foo:" isn't recognised.
    std::istringstream is("Foo: bar\r\n\r\nhi");
    CHECK_THROWS_AS(ReadFrame(is), std::runtime_error);
}

TEST_CASE("ReadFrame strips a leading UTF-8 BOM") {
    // Some clients on Windows insert a BOM when piping JSON. nlohmann::json's
    // parser would otherwise throw on it.
    std::string framed = "\xEF\xBB\xBF";  // BOM
    framed += R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    framed += "\n";
    std::istringstream is(framed);
    auto raw = ReadFrame(is);
    REQUIRE(raw.has_value());
    auto parsed = json::parse(*raw);
    CHECK(parsed["id"] == 1);
}

TEST_CASE("Dispatch handles parse error path (Content-Length input)") {
    Server s;
    std::string framed = "Content-Length: 5\r\n\r\n{bad}";
    std::istringstream in(framed);
    std::ostringstream out;
    std::ostringstream log;
    s.Run(in, out, log);

    // Mirrors client format: input was Content-Length, response is too.
    auto raw = out.str();
    auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    auto body = json::parse(raw.substr(split + 4));
    CHECK(body["id"].is_null());
    REQUIRE(body.contains("error"));
    CHECK(body["error"]["code"] == static_cast<int>(ErrorCode::ParseError));
}

TEST_CASE("Dispatch handles parse error path (newline-delimited input)") {
    Server s;
    std::string framed = "{bad}\n";
    std::istringstream in(framed);
    std::ostringstream out;
    std::ostringstream log;
    s.Run(in, out, log);

    // Mirrors client format: newline-delimited reply.
    auto raw = out.str();
    REQUIRE(!raw.empty());
    REQUIRE(raw.back() == '\n');
    auto body = json::parse(raw.substr(0, raw.size() - 1));
    CHECK(body["id"].is_null());
    REQUIRE(body.contains("error"));
    CHECK(body["error"]["code"] == static_cast<int>(ErrorCode::ParseError));
}

TEST_CASE("Dispatch returns method-not-found for unknown method") {
    Server s;
    json req = {{"jsonrpc", "2.0"}, {"method", "no_such_method"}, {"id", 7}};
    auto resp = s.Dispatch(req);
    REQUIRE(resp.has_value());
    CHECK((*resp)["id"] == 7);
    CHECK((*resp)["error"]["code"] == static_cast<int>(ErrorCode::MethodNotFound));
}

TEST_CASE("Dispatch invokes handler and returns its result") {
    Server s;
    s.Register("echo", [](const json& params) -> Response {
        return Response::Ok(params);
    });
    json req = {{"jsonrpc", "2.0"}, {"method", "echo"}, {"id", "abc"},
                {"params", json{{"x", 1}}}};
    auto resp = s.Dispatch(req);
    REQUIRE(resp.has_value());
    CHECK((*resp)["id"] == "abc");
    CHECK((*resp)["result"]["x"] == 1);
}

TEST_CASE("Dispatch swallows responses for notifications") {
    Server s;
    bool called = false;
    s.Register("notify", [&](const json&) -> Response {
        called = true;
        return Response::Ok({});
    });
    json req = {{"jsonrpc", "2.0"}, {"method", "notify"}}; // no id
    auto resp = s.Dispatch(req);
    CHECK_FALSE(resp.has_value());
    CHECK(called);
}

TEST_CASE("Dispatch surfaces handler exceptions as InternalError") {
    Server s;
    s.Register("bang", [](const json&) -> Response {
        throw std::runtime_error("boom");
    });
    json req = {{"jsonrpc", "2.0"}, {"method", "bang"}, {"id", 1}};
    auto resp = s.Dispatch(req);
    REQUIRE(resp.has_value());
    CHECK((*resp)["error"]["code"] == static_cast<int>(ErrorCode::InternalError));
    CHECK((*resp)["error"]["message"].get<std::string>().find("boom") != std::string::npos);
}

TEST_CASE("Dispatch rejects missing jsonrpc version") {
    Server s;
    json req = {{"method", "foo"}, {"id", 1}};
    auto resp = s.Dispatch(req);
    REQUIRE(resp.has_value());
    CHECK((*resp)["error"]["code"] == static_cast<int>(ErrorCode::InvalidRequest));
}
