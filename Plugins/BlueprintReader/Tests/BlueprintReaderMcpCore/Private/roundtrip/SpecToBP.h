// SpecToBP — drives IBlueprintReader write methods to rebuild a BP from a
// BPSpec. Halts on first write failure and surfaces which op failed.
#pragma once

#include <string>
#include <string_view>

#include "BPSpec.h"
#include "backends/IBlueprintReader.h"

namespace bpr::roundtrip {

struct SpecToBPResult {
	bool ok = false;
	std::string output_package_path;
	std::string failing_stage;   // empty on success
	std::string failing_op;
	std::string error_message;
};

// Creates `outputPackagePath` if missing, then issues writes through
// `reader` to materialize `spec`. Halts on first failure.
SpecToBPResult SpecToBP(backends::IBlueprintReader& reader,
						const BPSpec& spec,
						std::string_view outputPackagePath);

}    // namespace bpr::roundtrip
