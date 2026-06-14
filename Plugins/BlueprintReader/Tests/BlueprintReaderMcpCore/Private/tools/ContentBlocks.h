// Helpers for building MCP tool-result content blocks per the
// 2025-06-18 spec: https://modelcontextprotocol.io/specification/2025-06-18/server/tools
//
// Tools that want to emit text-only JSON keep returning plain JSON — the
// MCP layer dumps it to a single text content block (the historical
// behaviour). Tools that want richer output (images, audio, mixed
// content, structured-content-sibling-to-content) return an envelope
// produced by `Envelope(...)`. The dispatcher in Mcp.cpp detects the
// `_mcp` sentinel key and unpacks it directly into the tools/call
// response.
//
// Audience annotations let a tool say "this block is for the user" vs
// "this block is for the assistant" (or both). Clients that respect
// `annotations.audience` render only what's targeted. The default is
// "both" — matches today's text-result behaviour where the same text
// is what the user sees and what the assistant parses.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bpr::tools::content {

// Per MCP 2025-06-18 `annotations.audience`: a content block can be
// targeted at "user" (rendered in the UI), "assistant" (kept in the
// LLM context but not shown), or both (default).
enum class Audience : uint8_t {
	Both = 0,
	User = 1,
	Assistant = 2,
};

// Create a `type: "text"` content block. Optional audience annotation.
nlohmann::json Text(std::string text, Audience aud = Audience::Both);

// Create a `type: "image"` content block from already-base64-encoded data.
// `mime_type` is per MCP spec — e.g. "image/png", "image/jpeg".
// Default audience is User (image renders to user-facing UI).
nlohmann::json ImageBase64(std::string base64, std::string mime_type,
						   Audience aud = Audience::User);

// Create a `type: "image"` content block from raw bytes. Encodes to
// base64 internally. Use ImageBase64 if you already have the encoded form.
nlohmann::json Image(const std::vector<uint8_t>& bytes, std::string mime_type,
					 Audience aud = Audience::User);

// Build the rich-content envelope a tool returns to opt out of the
// default "dump to text" behaviour.
//
//   blocks            — array of content blocks (Text/Image/Audio/...)
//   structured        — optional structuredContent (must match the
//                       tool's declared outputSchema when set)
//
// The MCP layer inspects the `_mcp` key and unpacks it into the
// tools/call response. Existing tools that don't use this stay on the
// classic text-dump path.
nlohmann::json Envelope(std::vector<nlohmann::json> blocks,
						nlohmann::json structured = nlohmann::json());

}  // namespace bpr::tools::content
