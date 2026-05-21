// Opaque base64 cursors for paginated list_* tools and the MCP
// `tools/list` / `resources/list` primitives.
//
// Why opaque: the MCP spec wants cursors to be transparent only to
// the server. Encoding the offset as a clear integer would tempt
// clients to skip the cursor round-trip; the base64 wrap signals
// "treat this as a token, send it back as-is".
//
// Internally the cursor carries a single offset integer encoded as
// `{"o":<N>}` JSON then base64'd. Future revisions can add fields
// (filter signature, list epoch) without breaking decoders that only
// know about `o`. Invalid base64, invalid JSON, or missing `o` all
// yield std::nullopt — callers convert that to a JSON-RPC
// InvalidParams error.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bpr::tools {

// Encode a 0-based offset to an opaque base64 cursor.
std::string EncodeCursor(std::int64_t offset);

// Decode a cursor produced by EncodeCursor. Returns std::nullopt on
// malformed input (wrong base64, wrong JSON shape, negative offset).
std::optional<std::int64_t> DecodeCursor(std::string_view cursor);

}    // namespace bpr::tools
