#include "jsonrpc/HttpServerMain.h"

#include "jsonrpc/HttpTransport.h"
#include "jsonrpc/Server.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ostream>
#include <string>

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

	for (;;) {
		const SocketType conn = ::accept(listenSock, nullptr, nullptr);
		if (conn == kInvalidSocket) {
			continue;    // transient accept error — keep serving
		}
		const std::string raw = ReadRequest(conn);
		if (!raw.empty()) {
			HttpResponse resp;
			try {
				const HttpRequest req = ParseRequest(raw);
				resp = Handle(req, server, mcpPath);
			} catch (const std::exception& e) {
				resp.statusCode = 400;
				resp.statusText = "Bad Request";
				resp.contentType = "application/json";
				resp.body = std::string("{\"error\":\"") + e.what() + "\"}";
			}
			const std::string out = FormatResponse(resp);
			SendAll(conn, out.data(), out.size());
		}
		CloseSocket(conn);
	}
	// Unreachable in v1 (no in-band shutdown). Listener + WSACleanup are
	// reclaimed on process exit.
}

}    // namespace bpr::jsonrpc::http
