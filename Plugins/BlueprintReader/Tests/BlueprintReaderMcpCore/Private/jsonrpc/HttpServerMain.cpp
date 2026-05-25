#include "jsonrpc/HttpServerMain.h"

#include "jsonrpc/HttpTransport.h"
#include "jsonrpc/Server.h"
#include "jsonrpc/SseFrame.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "Ws2_32.lib")
	using SocketType = SOCKET;
	static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
	static void CloseSocket(SocketType s) { closesocket(s); }
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
	using SocketType = int;
	static constexpr SocketType kInvalidSocket = -1;
	static void CloseSocket(SocketType s) { ::close(s); }
#endif

namespace bpr::jsonrpc::http {

namespace {

bool SendAll(SocketType s, const char* data, std::size_t len) {
	std::size_t sent = 0;
	while (sent < len) {
		const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(n);
	}
	return true;
}

// Read one full HTTP request: headers up to the blank line, then
// Content-Length more body bytes. Returns the raw request text (empty on
// closed/error before any data). Caps total size to avoid unbounded read.
std::string ReadRequest(SocketType s) {
	constexpr std::size_t kMaxRequest = 16u * 1024u * 1024u;   // 16 MiB
	std::string buf;
	char chunk[4096];

	// Phase 1: read until the header terminator (CRLFCRLF or LFLF).
	std::size_t headerEnd = std::string::npos;
	std::size_t sepLen = 0;
	while (headerEnd == std::string::npos) {
		const int n = ::recv(s, chunk, sizeof(chunk), 0);
		if (n <= 0) {
			return buf;
		}
		buf.append(chunk, static_cast<std::size_t>(n));
		if (const std::size_t p = buf.find("\r\n\r\n"); p != std::string::npos) {
			headerEnd = p;
			sepLen = 4;
		} else if (const std::size_t q = buf.find("\n\n"); q != std::string::npos) {
			headerEnd = q;
			sepLen = 2;
		}
		if (buf.size() > kMaxRequest) {
			return buf;
		}
	}

	// Phase 2: read Content-Length body bytes (if declared).
	const std::size_t bodyStart = headerEnd + sepLen;
	std::size_t contentLength = 0;
	{
		std::string head = buf.substr(0, headerEnd);
		for (char& c : head) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		const std::size_t p = head.find("content-length:");
		if (p != std::string::npos) {
			contentLength = static_cast<std::size_t>(
				std::strtoul(head.c_str() + p + std::strlen("content-length:"), nullptr, 10));
		}
	}
	const std::size_t needed = bodyStart + contentLength;
	while (buf.size() < needed && buf.size() <= kMaxRequest) {
		const int n = ::recv(s, chunk, sizeof(chunk), 0);
		if (n <= 0) {
			break;
		}
		buf.append(chunk, static_cast<std::size_t>(n));
	}
	return buf;
}

// Write one HTTP/1.1 chunked-transfer chunk: "<hex-size>\r\n<data>\r\n".
bool SendChunk(SocketType s, const std::string& data) {
	char hdr[32];
	const int hlen = std::snprintf(hdr, sizeof(hdr), "%zx\r\n", data.size());
	if (hlen <= 0) {
		return false;
	}
	return SendAll(s, hdr, static_cast<std::size_t>(hlen))
		&& SendAll(s, data.data(), data.size())
		&& SendAll(s, "\r\n", 2);
}

// SSE GET stream (C5): hold the socket open, draining server-queued
// notifications into chunked text/event-stream frames until the client
// disconnects. Server access is serialized by `mtx` (Server isn't
// thread-safe). One queue is shared across streams — for multiple SSE
// clients the first to drain wins (fan-out is a later refinement; the
// common case is a single client).
void StreamSse(SocketType s, Server& server, std::mutex& mtx) {
	static constexpr char kHead[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"Transfer-Encoding: chunked\r\n"
		"\r\n";
	if (!SendAll(s, kHead, sizeof(kHead) - 1)) {
		return;
	}
	// Advise reconnect cadence up front.
	if (!SendChunk(s, FormatRetryFrame(3000))) {
		return;
	}
	int idleTicks = 0;
	for (;;) {
		std::vector<nlohmann::json> notes;
		{
			std::lock_guard<std::mutex> lock(mtx);
			notes = server.TakePendingNotifications();
		}
		for (const nlohmann::json& note : notes) {
			if (!SendChunk(s, FormatSseFrame("message", note))) {
				return;
			}
			idleTicks = 0;
		}
		if (notes.empty() && ++idleTicks >= 20) {    // ~5s keep-alive
			if (!SendChunk(s, FormatCommentFrame("keep-alive"))) {
				return;
			}
			idleTicks = 0;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

// Per-connection worker (one detached thread each). GET on the MCP path
// becomes an SSE stream; everything else is a one-shot JSON-RPC dispatch.
void HandleConnection(SocketType conn, Server& server, std::mutex& mtx,
					  std::string mcpPath) {
	const std::string raw = ReadRequest(conn);
	if (raw.empty()) {
		CloseSocket(conn);
		return;
	}
	HttpRequest req;
	try {
		req = ParseRequest(raw);
	} catch (const std::exception&) {
		HttpResponse bad;
		bad.statusCode = 400;
		bad.statusText = "Bad Request";
		bad.body = "{\"error\":\"malformed request\"}";
		const std::string out = FormatResponse(bad);
		SendAll(conn, out.data(), out.size());
		CloseSocket(conn);
		return;
	}

	if (req.method == "GET" && req.path == mcpPath) {
		// SSE stream — Origin guard still applies (defense in depth).
		if (!IsOriginAllowed(req)) {
			HttpResponse forbidden;
			forbidden.statusCode = 403;
			forbidden.statusText = "Forbidden";
			forbidden.body = "{\"error\":\"origin not allowed\"}";
			const std::string out = FormatResponse(forbidden);
			SendAll(conn, out.data(), out.size());
		} else {
			StreamSse(conn, server, mtx);
		}
		CloseSocket(conn);
		return;
	}

	HttpResponse resp;
	{
		std::lock_guard<std::mutex> lock(mtx);
		resp = Handle(req, server, mcpPath);
	}
	const std::string out = FormatResponse(resp);
	SendAll(conn, out.data(), out.size());
	CloseSocket(conn);
}

}    // namespace

int RunHttpServer(Server& server, uint16_t port, const std::string& mcpPath,
                  std::ostream& log) {
#if defined(_WIN32)
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		log << "[bp-reader-mcp][http] WSAStartup failed\n";
		return 1;
	}
#endif

	const SocketType listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == kInvalidSocket) {
		log << "[bp-reader-mcp][http] socket() failed\n";
		return 1;
	}

	int reuse = 1;
	::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
				 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	// Localhost only — never bind 0.0.0.0. The transport is localhost-only
	// by design (no external-deploy auth); the Origin guard in Handle is
	// defense in depth on top of this.
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		log << "[bp-reader-mcp][http] bind 127.0.0.1:" << port << " failed\n";
		CloseSocket(listenSock);
		return 1;
	}
	if (::listen(listenSock, 16) != 0) {
		log << "[bp-reader-mcp][http] listen failed\n";
		CloseSocket(listenSock);
		return 1;
	}
	log << "[bp-reader-mcp][http] listening on http://127.0.0.1:" << port
		<< mcpPath << "\n";

	// Serializes all Server access across connection threads (Server is
	// single-threaded by design). Lives for the process — this function
	// never returns — so detached workers may safely capture it by ref.
	std::mutex serverMutex;

	for (;;) {
		const SocketType conn = ::accept(listenSock, nullptr, nullptr);
		if (conn == kInvalidSocket) {
			continue;    // transient accept error — keep serving
		}
		// One detached worker per connection so an SSE GET stream can be
		// held open while POSTs are served on other connections.
		std::thread(HandleConnection, conn, std::ref(server),
					std::ref(serverMutex), mcpPath).detach();
	}
	// Unreachable in v1 (no in-band shutdown). Listener + WSACleanup are
	// reclaimed on process exit.
}

}    // namespace bpr::jsonrpc::http
