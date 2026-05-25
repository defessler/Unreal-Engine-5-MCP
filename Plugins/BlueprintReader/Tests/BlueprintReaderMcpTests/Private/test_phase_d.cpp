// Phase D tests: outputSchema rollout + return_inline image content blocks.
//
// Covers:
//  - ImageReader::ReadPngDimensions[FromBytes] correctness on synthetic PNGs
//  - ToolRegistry default outputSchema fallback (every tool advertises one)
//  - take_screenshot / take_viewport_screenshot input_schema includes
//    return_inline; output_schema declares image_width/image_height
//  - BuildScreenshotResponse cap rejection + Envelope shape directly —
//    we hit this helper instead of the registered tool so the test
//    doesn't need a full IBlueprintReader implementation.

#include <doctest/doctest.h>

#include "backends/MockBlueprintReader.h"
#include "tools/BlueprintTools.h"
#include "tools/ImageReader.h"
#include "tools/ToolRegistry.h"

#include "test_helpers.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bpr;
using nlohmann::json;

namespace test_phase_d_detail {

// Build a minimal valid PNG header + IHDR chunk for (w, h). Only the
// dimensions and signature need to be right for ImageReader's parser —
// downstream chunks aren't read.
std::vector<uint8_t> SynthesizePngHeader(uint32_t w, uint32_t h) {
	std::vector<uint8_t> b;
	// 8-byte PNG signature
	b.insert(b.end(), {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
	// IHDR chunk length = 13 (big-endian)
	b.insert(b.end(), {0x00, 0x00, 0x00, 0x0D});
	// Chunk type "IHDR"
	b.insert(b.end(), {'I', 'H', 'D', 'R'});
	// Width (big-endian)
	b.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
	b.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
	b.push_back(static_cast<uint8_t>((w >>  8) & 0xFF));
	b.push_back(static_cast<uint8_t>( w        & 0xFF));
	// Height (big-endian)
	b.push_back(static_cast<uint8_t>((h >> 24) & 0xFF));
	b.push_back(static_cast<uint8_t>((h >> 16) & 0xFF));
	b.push_back(static_cast<uint8_t>((h >>  8) & 0xFF));
	b.push_back(static_cast<uint8_t>( h        & 0xFF));
	// Bit depth, color type, compression, filter, interlace + CRC —
	// pad to 9 bytes to round out the IHDR body (5 bytes) + CRC (4 bytes).
	// Total file is 33 bytes — small + deterministic.
	b.insert(b.end(), {8, 6, 0, 0, 0, 0, 0, 0, 0});
	return b;
}

// Write `bytes` to a fresh temp file. Returns the path. Caller is
// responsible for cleanup.
std::filesystem::path WriteTempPng(const std::vector<uint8_t>& bytes,
								   const std::string& tag) {
	const auto base = std::filesystem::temp_directory_path();
	const auto path = base / ("bpr-test-" + tag + ".png");
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()),
			  static_cast<std::streamsize>(bytes.size()));
	out.close();
	return path;
}

}    // namespace test_phase_d_detail
using namespace test_phase_d_detail;

// =====================================================================
// ImageReader unit tests
// =====================================================================

TEST_CASE("ImageReader: parses synthetic PNG header (640x480)") {
	auto bytes = SynthesizePngHeader(640, 480);
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	REQUIRE(dims.has_value());
	CHECK(dims->width  == 640);
	CHECK(dims->height == 480);
}

TEST_CASE("ImageReader: parses cap-edge (1280x1280)") {
	auto bytes = SynthesizePngHeader(1280, 1280);
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	REQUIRE(dims.has_value());
	CHECK(dims->width  == 1280);
	CHECK(dims->height == 1280);
}

TEST_CASE("ImageReader: parses non-square (1920x1080)") {
	auto bytes = SynthesizePngHeader(1920, 1080);
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	REQUIRE(dims.has_value());
	CHECK(dims->width  == 1920);
	CHECK(dims->height == 1080);
}

TEST_CASE("ImageReader: rejects empty buffer") {
	std::vector<uint8_t> empty;
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(empty);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: rejects truncated buffer (<24 bytes)") {
	std::vector<uint8_t> trunc = {0x89, 0x50, 0x4E, 0x47, 0x0D};
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(trunc);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: rejects buffer without PNG signature") {
	std::vector<uint8_t> notPng(24, 0xFF);
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(notPng);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: rejects PNG with wrong IHDR length") {
	auto bytes = SynthesizePngHeader(100, 100);
	// Corrupt the IHDR length field to 12 (spec mandates 13).
	bytes[11] = 0x0C;
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: rejects PNG with non-IHDR first chunk") {
	auto bytes = SynthesizePngHeader(100, 100);
	bytes[12] = 'X';
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: rejects PNG with zero dimensions") {
	auto bytes = SynthesizePngHeader(0, 100);
	auto dims = tools::imageio::ReadPngDimensionsFromBytes(bytes);
	CHECK_FALSE(dims.has_value());
}

TEST_CASE("ImageReader: reads dimensions from a file on disk") {
	auto bytes = SynthesizePngHeader(512, 384);
	auto path = WriteTempPng(bytes, "diskread");
	auto dims = tools::imageio::ReadPngDimensions(path);
	REQUIRE(dims.has_value());
	CHECK(dims->width  == 512);
	CHECK(dims->height == 384);
	std::filesystem::remove(path);
}

TEST_CASE("ImageReader: returns nullopt for nonexistent file") {
	auto dims = tools::imageio::ReadPngDimensions(
		std::filesystem::path("Z:/does-not-exist/bpr-no-such.png"));
	CHECK_FALSE(dims.has_value());
}

// =====================================================================
// ToolRegistry default outputSchema fallback
// =====================================================================

TEST_CASE("ToolRegistry: every tool advertises an outputSchema in ListSpec") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	REQUIRE(spec.is_array());
	REQUIRE(spec.size() == 222);
	for (const auto& t : spec) {
		CAPTURE(t["name"].get<std::string>());
		REQUIRE(t.contains("outputSchema"));
		REQUIRE(t["outputSchema"].is_object());
		REQUIRE(t["outputSchema"].contains("type"));
		const auto typ = t["outputSchema"]["type"].get<std::string>();
		CHECK((typ == "object" || typ == "array"));
	}
}

TEST_CASE("ToolRegistry: list_* tools default to outputSchema.type=array") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		const std::string name = t["name"].get<std::string>();
		if (name == "list_blueprints" || name == "list_data_tables" ||
			name == "list_variables"  || name == "list_node_kinds"  ||
			name == "list_assets") {
			CAPTURE(name);
			CHECK(t["outputSchema"]["type"] == "array");
		}
	}
}

TEST_CASE("ToolRegistry: object-shaped tools default to outputSchema.type=object") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		const std::string name = t["name"].get<std::string>();
		if (name == "find_class" || name == "save_all" ||
			name == "compile_function" || name == "create_blueprint") {
			CAPTURE(name);
			CHECK(t["outputSchema"]["type"] == "object");
		}
	}
}

TEST_CASE("ToolRegistry: explicit output_schema takes precedence over default") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "take_viewport_screenshot") {
			REQUIRE(t["outputSchema"]["type"] == "object");
			REQUIRE(t["outputSchema"].contains("properties"));
			CHECK(t["outputSchema"]["properties"].contains("output_file"));
			return;
		}
	}
	FAIL("take_viewport_screenshot not found in tools list");
}

// =====================================================================
// Screenshot tool schema declarations
// =====================================================================

TEST_CASE("take_screenshot: input_schema declares return_inline option") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "take_screenshot") {
			const auto& props = t["inputSchema"]["properties"];
			REQUIRE(props.contains("return_inline"));
			CHECK(props["return_inline"]["type"] == "boolean");
			return;
		}
	}
	FAIL("take_screenshot not found");
}

TEST_CASE("take_viewport_screenshot: input_schema declares return_inline option") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "take_viewport_screenshot") {
			const auto& props = t["inputSchema"]["properties"];
			REQUIRE(props.contains("return_inline"));
			CHECK(props["return_inline"]["type"] == "boolean");
			return;
		}
	}
	FAIL("take_viewport_screenshot not found");
}

TEST_CASE("take_screenshot: output_schema includes image_width / image_height") {
	auto reader = test::MakeMockReader();
	tools::ToolRegistry registry;
	tools::RegisterBlueprintTools(registry, reader);
	auto spec = registry.ListSpec();
	for (const auto& t : spec) {
		if (t["name"] == "take_screenshot") {
			REQUIRE(t["outputSchema"].contains("properties"));
			const auto& props = t["outputSchema"]["properties"];
			CHECK(props.contains("image_width"));
			CHECK(props.contains("image_height"));
			return;
		}
	}
	FAIL("take_screenshot not found");
}

// =====================================================================
// BuildScreenshotResponse direct tests
// =====================================================================

TEST_CASE("BuildScreenshotResponse: return_inline=false returns classic shape") {
	// File doesn't even need to exist when return_inline is false —
	// the helper short-circuits before touching the disk.
	auto out = tools::BuildScreenshotResponse(
		"/tmp/whatever.png", /*captured=*/true,
		"/tmp/canonical.png", /*returnInline=*/false);
	CHECK_FALSE(out.contains("_mcp"));
	CHECK(out["ok"] == true);
	CHECK(out["captured"] == true);
	CHECK(out["output_file"] == "/tmp/canonical.png");
}

TEST_CASE("BuildScreenshotResponse: captured=false skips inline emission") {
	// When the underlying tool reports captured=false, don't try to
	// read a file that probably doesn't exist — return classic shape.
	auto out = tools::BuildScreenshotResponse(
		"/tmp/whatever.png", /*captured=*/false,
		"/tmp/canonical.png", /*returnInline=*/true);
	CHECK_FALSE(out.contains("_mcp"));
	CHECK(out["captured"] == false);
}

TEST_CASE("BuildScreenshotResponse: return_inline=true emits _mcp envelope with image block") {
	auto fixture = WriteTempPng(SynthesizePngHeader(640, 480), "inline-on");
	auto out = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	REQUIRE(out.contains("_mcp"));
	REQUIRE(out["_mcp"].contains("content"));
	const auto& blocks = out["_mcp"]["content"];
	REQUIRE(blocks.is_array());
	REQUIRE(blocks.size() >= 1);
	CHECK(blocks[0]["type"] == "image");
	CHECK(blocks[0]["mimeType"] == "image/png");
	CHECK(blocks[0]["data"].is_string());
	CHECK(blocks[0]["data"].get<std::string>().size() > 0);
	REQUIRE(out["_mcp"].contains("structuredContent"));
	CHECK(out["_mcp"]["structuredContent"]["ok"] == true);
	CHECK(out["_mcp"]["structuredContent"]["image_width"]  == 640);
	CHECK(out["_mcp"]["structuredContent"]["image_height"] == 480);
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: rejects PNG over 1280px cap") {
	auto fixture = WriteTempPng(SynthesizePngHeader(2560, 1440), "inline-toobig");
	try {
		tools::BuildScreenshotResponse(
			fixture.string(), true, fixture.string(), true);
		FAIL("expected invalid_argument for over-cap PNG");
	} catch (const std::invalid_argument& e) {
		const std::string msg = e.what();
		CHECK(msg.find("1280") != std::string::npos);
		CHECK(msg.find("2560") != std::string::npos);
	}
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: accepts cap-edge PNG (1280x1280)") {
	auto fixture = WriteTempPng(SynthesizePngHeader(1280, 1280), "inline-edge");
	auto out = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	REQUIRE(out.contains("_mcp"));
	CHECK(out["_mcp"]["structuredContent"]["image_width"]  == 1280);
	CHECK(out["_mcp"]["structuredContent"]["image_height"] == 1280);
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: rejects missing file when inline requested") {
	CHECK_THROWS_AS(tools::BuildScreenshotResponse(
		"Z:/does-not-exist/bpr-test-missing.png", /*captured=*/true,
		"Z:/does-not-exist/bpr-test-missing.png", /*returnInline=*/true),
		std::invalid_argument);
}

TEST_CASE("BuildScreenshotResponse: rejects non-PNG file when inline requested") {
	const auto base = std::filesystem::temp_directory_path();
	const auto path = base / "bpr-test-notpng.png";
	{
		std::ofstream out(path, std::ios::binary);
		std::string garbage(64, 'X');
		out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
	}
	try {
		tools::BuildScreenshotResponse(
			path.string(), true, path.string(), true);
		FAIL("expected invalid_argument for non-PNG file");
	} catch (const std::invalid_argument& e) {
		const std::string msg = e.what();
		CHECK(msg.find("IHDR") != std::string::npos);
	}
	std::filesystem::remove(path);
}

TEST_CASE("BuildScreenshotResponse: image content block carries audience=user annotation") {
	auto fixture = WriteTempPng(SynthesizePngHeader(640, 480), "audience");
	auto out = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	REQUIRE(out.contains("_mcp"));
	const auto& blocks = out["_mcp"]["content"];
	REQUIRE(blocks.size() >= 1);
	const auto& imageBlock = blocks[0];
	CHECK(imageBlock["type"] == "image");
	REQUIRE(imageBlock.contains("annotations"));
	REQUIRE(imageBlock["annotations"].contains("audience"));
	const auto& aud = imageBlock["annotations"]["audience"];
	REQUIRE(aud.is_array());
	CHECK(aud.size() == 1);
	CHECK(aud[0] == "user");
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: deterministic base64 from a fixed fixture") {
	// Same input bytes → same base64. Covers the "deterministic
	// checksum on test fixture" Phase D exit criterion.
	auto bytes = SynthesizePngHeader(8, 8);
	auto fixture = WriteTempPng(bytes, "deterministic");
	auto out1 = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	auto out2 = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	REQUIRE(out1["_mcp"]["content"][0].contains("data"));
	REQUIRE(out2["_mcp"]["content"][0].contains("data"));
	CHECK(out1["_mcp"]["content"][0]["data"] ==
		  out2["_mcp"]["content"][0]["data"]);
	// 33 raw bytes → 44 base64 chars (ceil(33/3)*4)
	const auto& b64 = out1["_mcp"]["content"][0]["data"].get<std::string>();
	CHECK(b64.size() == 44);
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: structuredContent mirrors classic response keys") {
	auto fixture = WriteTempPng(SynthesizePngHeader(320, 240), "structured");
	auto out = tools::BuildScreenshotResponse(
		fixture.string(), true, fixture.string(), true);
	REQUIRE(out["_mcp"].contains("structuredContent"));
	const auto& sc = out["_mcp"]["structuredContent"];
	CHECK(sc["ok"] == true);
	CHECK(sc["captured"] == true);
	CHECK(sc["output_file"] == fixture.string());
	CHECK(sc.contains("image_width"));
	CHECK(sc.contains("image_height"));
	std::filesystem::remove(fixture);
}

TEST_CASE("BuildScreenshotResponse: falls back to dest_path when outputFile empty") {
	// Some backends leave outputFile empty when the tool wrote to the
	// requested dest_path verbatim. The helper should still find the file.
	auto fixture = WriteTempPng(SynthesizePngHeader(320, 240), "fallback");
	auto out = tools::BuildScreenshotResponse(
		fixture.string(), true, /*outputFile=*/"", /*returnInline=*/true);
	REQUIRE(out.contains("_mcp"));
	CHECK(out["_mcp"]["structuredContent"]["image_width"] == 320);
	std::filesystem::remove(fixture);
}
