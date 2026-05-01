// Tests for the JSON-RPC framing + dispatch layer.

#include <doctest/doctest.h>

#include "jsonrpc/Server.h"

#include <sstream>

using namespace bpr::jsonrpc;
using nlohmann::json;

namespace {

std::string Frame(const json& body) {
    std::ostringstream os;
    WriteFrame(os, body);
    return os.str();
}

} // namespace

TEST_CASE("WriteFrame emits Content-Length header and body") {
    json body = {{"hello", "world"}};
    auto out = Frame(body);
    auto headerEnd = out.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    auto headers = out.substr(0, headerEnd);
    auto payload = out.substr(headerEnd + 4);
    CHECK(headers.find("Content-Length: ") == 0);
    auto parsed = json::parse(payload);
    CHECK(parsed["hello"] == "world");
}

TEST_CASE("ReadFrame round-trips with WriteFrame") {
    json body = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
    auto framed = Frame(body);
    std::istringstream is(framed);
    auto raw = ReadFrame(is);
    REQUIRE(raw.has_value());
    CHECK(json::parse(*raw) == body);
    // Second read returns nullopt (EOF).
    CHECK_FALSE(ReadFrame(is).has_value());
}

TEST_CASE("ReadFrame errors on missing Content-Length") {
    std::istringstream is("Foo: bar\r\n\r\nhi");
    CHECK_THROWS_AS(ReadFrame(is), std::runtime_error);
}

TEST_CASE("Dispatch handles parse error path") {
    Server s;
    // Build a syntactically invalid JSON via the public Run() loop is not
    // possible without IO; we exercise Server::Run via streams instead.
    std::string framed = "Content-Length: 5\r\n\r\n{bad}";
    std::istringstream in(framed);
    std::ostringstream out;
    std::ostringstream log;
    s.Run(in, out, log);

    // Output should contain a single framed error envelope with id = null.
    auto raw = out.str();
    auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    auto body = json::parse(raw.substr(split + 4));
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
