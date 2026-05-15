# Chapter 1 — Foundation: a bare MCP server in C++

You're going to build a Model Context Protocol (MCP) server from nothing.
By the end of this chapter you'll have a C++20 executable that reads
JSON-RPC frames from stdin, writes responses to stdout, and answers two
of MCP's required methods: `initialize` and `tools/list`. You won't talk
to Unreal yet — that arrives in chapter 3. For now the goal is a clean
JSON-RPC loop you can drive from a shell.

> **Build system:** this chapter uses CMake to keep the early steps
> small and incremental. The shipping project later switched to a UE
> Program target (UBT) so the MCP server builds in the same pipeline as
> the rest of the plugin — see the [tutorial README](README.md) for
> context and [design/04-mcp-server.md](../design/04-mcp-server.md) for
> the current production layout. The source code stays C++20 stdlib + a
> handful of header-only deps in both setups; only the build commands
> differ.

## What MCP actually is

MCP is a thin wrapper around JSON-RPC 2.0. A client (Claude Desktop,
Claude Code, an IDE) launches your server as a subprocess and exchanges
newline-delimited JSON messages over stdio. There are three message
types you need to know:

- **Request** — `{"jsonrpc":"2.0","id":N,"method":"...","params":{...}}`.
  Always carries an `id`. Expects a response with the same `id`.
- **Response** — `{"jsonrpc":"2.0","id":N,"result":{...}}` on success,
  or `{"jsonrpc":"2.0","id":N,"error":{"code":N,"message":"..."}}` on
  failure.
- **Notification** — same shape as a request but with no `id`. Fire-
  and-forget; do not reply.

A real handshake looks like this. Client sends:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocolVersion":"2024-11-05",
  "capabilities":{},
  "clientInfo":{"name":"claude-code","version":"1.0.0"}
}}
```

Server replies:

```json
{"jsonrpc":"2.0","id":1,"result":{
  "protocolVersion":"2024-11-05",
  "capabilities":{"tools":{}},
  "serverInfo":{"name":"bp-reader-mcp","version":"0.1.0"}
}}
```

Then a notification (no reply expected):

```json
{"jsonrpc":"2.0","method":"notifications/initialized"}
```

After that the client typically calls `tools/list` to discover what
your server exposes, then `tools/call` to invoke individual tools.
Chapter 2 covers `tools/call`. This chapter stops at `tools/list`.

For the full type system rationale and wire-format decisions, see
`../design/06-wire-protocol.md` once you reach that chapter. For now,
the four methods you'll handle are:

| Method                       | Type         | Returns                       |
|------------------------------|--------------|-------------------------------|
| `initialize`                 | request      | server info + capabilities    |
| `notifications/initialized`  | notification | (nothing)                     |
| `ping`                       | request      | empty object                  |
| `tools/list`                 | request      | array of tool descriptors     |

## Project layout

You'll build a standalone server — no UE dependency yet, no plugin
wrapper. Just a C++ project with vendored deps.

```
bp-reader/
├── CMakeLists.txt
├── third_party/
│   ├── nlohmann_json/        # nlohmann/json.hpp (single header)
│   ├── fmt/                  # fmt/core.h + format.h (header-only)
│   └── doctest/              # doctest.h (single header)
├── src/
│   ├── main.cpp
│   └── jsonrpc/
│       ├── Server.h
│       └── Server.cpp
└── tests/
    └── test_server.cpp
```

Vendoring is deliberate. The real BlueprintReader server vendors every
dep so a fresh clone builds without `vcpkg`, `FetchContent`, or any
network access (see `Plugins/BlueprintReader/Tests/ThirdParty/`
for the production layout). Download these headers once and commit
them to your tree:

- `nlohmann_json` — https://github.com/nlohmann/json/releases — grab
  `json.hpp` and place it at `third_party/nlohmann_json/nlohmann/json.hpp`.
- `fmt` — https://github.com/fmtlib/fmt/releases — copy the entire
  `include/fmt/` directory to `third_party/fmt/fmt/`.
- `doctest` — https://github.com/doctest/doctest — `doctest/doctest.h`.

## CMake scaffold

Drop this in `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.23)
project(bp-reader-mcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_definitions(-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- vendored deps as INTERFACE targets ------------------------------------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json)

add_library(fmt INTERFACE)
target_include_directories(fmt INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt)
target_compile_definitions(fmt INTERFACE FMT_HEADER_ONLY=1)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)

# --- core library ----------------------------------------------------------
add_library(bp-reader-core STATIC
    src/jsonrpc/Server.cpp
)
target_include_directories(bp-reader-core PUBLIC src)
target_link_libraries(bp-reader-core PUBLIC nlohmann_json fmt)

# --- executable ------------------------------------------------------------
add_executable(bp-reader-mcp src/main.cpp)
target_link_libraries(bp-reader-mcp PRIVATE bp-reader-core)

enable_testing()
add_subdirectory(tests)
```

This is the same skeleton the production server uses (compare to
`(deleted: replaced by Plugins/BlueprintReader/Tests/BlueprintReader*/*.Build.cs)`), minus the
warning-as-error knobs and the many source files that grow in later
chapters.

Tests get their own `CMakeLists.txt`:

```cmake
# tests/CMakeLists.txt
add_executable(bp-reader-tests test_server.cpp)
target_link_libraries(bp-reader-tests PRIVATE bp-reader-core doctest)
add_test(NAME bp-reader-tests COMMAND bp-reader-tests)
```

## The framing rule

JSON-RPC says nothing about framing. MCP picks one: **newline-delimited
JSON, one object per line, no embedded newlines.** That's it. Your
server reads until `\n`, parses the body, dispatches, writes the
response followed by `\n`, flushes.

Some early MCP clients (the LSP-derived ones) speak `Content-Length`-
framed JSON instead. Production code auto-detects on the first byte
(`{` or `[` means newline-delimited; anything else means LSP headers).
You won't bother in chapter 1 — newline-only.

## The server skeleton

`src/jsonrpc/Server.h`:

```cpp
#pragma once

#include <functional>
#include <istream>
#include <ostream>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace bpr::jsonrpc {

enum class ErrorCode : int {
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,
};

struct Error {
    int code;
    std::string message;
};

struct Response {
    std::optional<nlohmann::json> result;
    std::optional<Error>          error;

    static Response Ok(nlohmann::json r)    { return {std::move(r), std::nullopt}; }
    static Response Fail(ErrorCode c, std::string m) {
        return {std::nullopt, Error{static_cast<int>(c), std::move(m)}};
    }
};

class Server {
public:
    using Handler = std::function<Response(const nlohmann::json& params)>;
    void Register(std::string method, Handler h);
    void Run(std::istream& in, std::ostream& out, std::ostream& log);

private:
    std::optional<nlohmann::json> Dispatch(const nlohmann::json& body);
    std::unordered_map<std::string, Handler> handlers_;
};

} // namespace bpr::jsonrpc
```

`src/jsonrpc/Server.cpp` — keep it short for now. The production version
in `Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/jsonrpc/Server.cpp` adds BOM
skipping, batch support, and LSP framing; you can graduate to those
later.

```cpp
#include "jsonrpc/Server.h"
#include <fmt/core.h>

namespace bpr::jsonrpc {

void Server::Register(std::string method, Handler h) {
    handlers_[std::move(method)] = std::move(h);
}

static nlohmann::json MakeError(const nlohmann::json& id, int code,
                                const std::string& msg) {
    return {{"jsonrpc","2.0"}, {"id",id},
            {"error", {{"code",code}, {"message",msg}}}};
}

std::optional<nlohmann::json> Server::Dispatch(const nlohmann::json& body) {
    if (!body.is_object()) {
        return MakeError(nullptr, (int)ErrorCode::InvalidRequest,
                         "request must be an object");
    }
    auto idIt = body.find("id");
    nlohmann::json id = (idIt != body.end()) ? *idIt : nlohmann::json(nullptr);
    const bool isNotif = (idIt == body.end());

    std::string method;
    if (auto m = body.find("method"); m != body.end() && m->is_string()) {
        method = m->get<std::string>();
    } else {
        return isNotif ? std::nullopt
            : std::optional(MakeError(id, (int)ErrorCode::InvalidRequest,
                                      "missing method"));
    }

    nlohmann::json params = nlohmann::json::object();
    if (auto p = body.find("params"); p != body.end()) params = *p;

    auto h = handlers_.find(method);
    if (h == handlers_.end()) {
        return isNotif ? std::nullopt
            : std::optional(MakeError(id, (int)ErrorCode::MethodNotFound,
                                      fmt::format("method not found: {}", method)));
    }

    Response r;
    try { r = h->second(params); }
    catch (const std::exception& e) {
        return isNotif ? std::nullopt
            : std::optional(MakeError(id, (int)ErrorCode::InternalError,
                                      fmt::format("handler threw: {}", e.what())));
    }

    if (isNotif) return std::nullopt;
    if (r.error) return MakeError(id, r.error->code, r.error->message);

    return nlohmann::json{
        {"jsonrpc","2.0"}, {"id",id},
        {"result", r.result.value_or(nlohmann::json::object())}};
}

void Server::Run(std::istream& in, std::ostream& out, std::ostream& log) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        nlohmann::json body;
        try { body = nlohmann::json::parse(line); }
        catch (const std::exception& e) {
            out << MakeError(nullptr, (int)ErrorCode::ParseError, e.what()).dump()
                << '\n';
            out.flush();
            continue;
        }
        if (auto reply = Dispatch(body)) {
            out << reply->dump() << '\n';
            out.flush();
        }
    }
    (void)log;
}

} // namespace bpr::jsonrpc
```

## main.cpp — wire up the four methods

```cpp
#include "jsonrpc/Server.h"
#include <iostream>

int main() {
    bpr::jsonrpc::Server server;

    server.Register("initialize",
        [](const nlohmann::json& /*params*/) {
            return bpr::jsonrpc::Response::Ok({
                {"protocolVersion", "2024-11-05"},
                {"capabilities", {{"tools", nlohmann::json::object()}}},
                {"serverInfo", {{"name", "bp-reader-mcp"},
                                {"version", "0.1.0"}}},
            });
        });

    server.Register("notifications/initialized",
        [](const nlohmann::json&) {
            return bpr::jsonrpc::Response::Ok(nlohmann::json::object());
        });

    server.Register("ping",
        [](const nlohmann::json&) {
            return bpr::jsonrpc::Response::Ok(nlohmann::json::object());
        });

    server.Register("tools/list",
        [](const nlohmann::json&) {
            // No tools yet. Chapter 2 wires this to a real registry.
            return bpr::jsonrpc::Response::Ok({
                {"tools", nlohmann::json::array()}
            });
        });

    server.Run(std::cin, std::cout, std::cerr);
    return 0;
}
```

That `tools/list` returning an empty array is deliberate — it's a valid
response that lets a client connect without errors. Chapter 2 fills it
with real tools.

For comparison, the production `RegisterHandlers` lives in
`Plugins/BlueprintReader/Tests/BlueprintReaderMcpCore/Private/jsonrpc/Mcp.cpp` and is barely
longer than the snippet above. Most of the extra lines there negotiate
protocol versions across the three MCP spec revisions; you can ignore
that until you're shipping.

## Build

```pwsh
"<Engine>\Engine\Build\BatchFiles\Build.bat" BlueprintReaderMcp Win64 Development -project="<Project>.uproject"
# (same UBT command rebuilds incrementally)
```

On Linux or macOS, drop the generator flag — Ninja or Unix Makefiles
work fine.

## Checkpoint

Run the executable and feed it a JSON-RPC initialize request:

```pwsh
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' |
    .\build\Release\BlueprintReaderMcp.exe
```

Expect to see, on stdout:

```json
{"jsonrpc":"2.0","id":1,"result":{"capabilities":{"tools":{}},"protocolVersion":"2024-11-05","serverInfo":{"name":"bp-reader-mcp","version":"0.1.0"}}}
```

(Key order depends on `nlohmann::json`'s default sort — alphabetical.
Don't rely on the literal byte ordering, just the structure.)

Then try a sequence to confirm the loop survives multiple messages:

```pwsh
@'
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
'@ | .\build\Release\BlueprintReaderMcp.exe
```

Expect two responses (one for `id:1`, one for `id:2`) and silence for
the notification. If you see:

- **Nothing on stdout** — your handler probably crashed before reaching
  `Run`. Verify the binary built and runs (`BlueprintReaderMcp.exe --help`
  is a no-op but should exit cleanly).
- **A `method not found` error for `notifications/initialized`** —
  you forgot to register it, or you put an `id` on the notification in
  your test input (notifications must not have one).
- **A `parse error`** — your shell escaped quotes. PowerShell here-
  strings (`@'...'@`) preserve the JSON literally; bash needs single
  quotes around each line.

When all three messages produce the right shape, you're done with
chapter 1. Chapter 2 introduces the tool registry and a mock backend
so `tools/list` and `tools/call` return real data.
