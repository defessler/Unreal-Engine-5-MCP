#include "tools/ImageReader.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace bpr::tools::imageio {

namespace image_reader_detail {

// PNG signature: 137 80 78 71 13 10 26 10 (the "PNG" magic per RFC 2083).
constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47,
									  0x0D, 0x0A, 0x1A, 0x0A};

// Read a big-endian uint32 from `bytes` at `offset`. Caller must ensure
// offset+4 is in-bounds.
uint32_t ReadU32BE(const std::vector<uint8_t>& bytes, std::size_t offset) {
	return (static_cast<uint32_t>(bytes[offset    ]) << 24) |
	       (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
	       (static_cast<uint32_t>(bytes[offset + 2]) <<  8) |
	        static_cast<uint32_t>(bytes[offset + 3]);
}

}    // namespace image_reader_detail
using namespace image_reader_detail;

std::optional<PngDimensions> ReadPngDimensionsFromBytes(
	const std::vector<uint8_t>& bytes) {
	// PNG signature (8) + IHDR chunk length (4) + chunk type "IHDR" (4)
	// + width (4) + height (4) = 24 bytes minimum.
	if (bytes.size() < 24) {
		return std::nullopt;
	}
	for (int i = 0; i < 8; ++i) {
		if (bytes[i] != kPngSignature[i]) {
			return std::nullopt;
		}
	}
	// IHDR length field at offset 8; spec mandates IHDR length = 13.
	const uint32_t ihdrLen = ReadU32BE(bytes, 8);
	if (ihdrLen != 13) {
		return std::nullopt;
	}
	// Chunk type at offset 12 must be "IHDR".
	if (bytes[12] != 'I' || bytes[13] != 'H' ||
		bytes[14] != 'D' || bytes[15] != 'R') {
		return std::nullopt;
	}
	PngDimensions out;
	out.width  = ReadU32BE(bytes, 16);
	out.height = ReadU32BE(bytes, 20);
	if (out.width == 0 || out.height == 0) {
		return std::nullopt;
	}
	return out;
}

std::optional<PngDimensions> ReadPngDimensions(const std::filesystem::path& path) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		return std::nullopt;
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::nullopt;
	}
	std::vector<uint8_t> head(24);
	in.read(reinterpret_cast<char*>(head.data()), 24);
	if (in.gcount() < 24) {
		return std::nullopt;
	}
	return ReadPngDimensionsFromBytes(head);
}

}    // namespace bpr::tools::imageio
