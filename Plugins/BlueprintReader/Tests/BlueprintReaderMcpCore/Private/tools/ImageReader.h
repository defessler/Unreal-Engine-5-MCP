// Minimal PNG metadata reader. Parses the IHDR chunk so the screenshot
// tools can enforce the 1280px max-dim cap before base64-encoding a
// capture for inline emission. We don't decode pixels — UE writes the
// PNG and the MCP server just enforces a size policy on the disk
// artifact before it goes back over the wire.
//
// Why minimal: a full PNG decoder needs zlib + a non-trivial chunk
// state machine. For the cap check we only need the very first chunk
// (IHDR) which is uncompressed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace bpr::tools::imageio {

struct PngDimensions {
	uint32_t width  = 0;
	uint32_t height = 0;
};

// Parse the IHDR chunk of a PNG file on disk. Returns std::nullopt if:
//   - file doesn't exist or can't be read
//   - file isn't a PNG (signature mismatch)
//   - first chunk isn't IHDR (malformed PNG)
//   - file is truncated before the IHDR length+type+payload
//
// On success, returns the {width, height} from the IHDR chunk.
// Cheap: reads only the first 24 bytes of the file.
std::optional<PngDimensions> ReadPngDimensions(const std::filesystem::path& path);

// Same, but operates on an in-memory byte buffer. Used by tests + any
// caller that already has the bytes loaded.
std::optional<PngDimensions> ReadPngDimensionsFromBytes(
	const std::vector<uint8_t>& bytes);

}    // namespace bpr::tools::imageio
