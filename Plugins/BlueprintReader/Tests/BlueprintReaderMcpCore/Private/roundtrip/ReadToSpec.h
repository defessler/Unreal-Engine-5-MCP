// ReadToSpec — orchestrates IBlueprintReader read methods to assemble a
// complete BPSpec for one Blueprint. Pure: no I/O outside the reader.
#pragma once

#include <string_view>

#include "BPSpec.h"
#include "backends/IBlueprintReader.h"

namespace bpr::roundtrip {

// Drives `reader` to gather every piece of metadata for `assetPath` and
// assembles it into a BPSpec. On any error from the reader, sets
// `spec.incomplete = true` and appends the error to `spec.errors`, then
// returns whatever partial spec was built. Never throws.
BPSpec ReadToSpec(backends::IBlueprintReader& reader, std::string_view assetPath);

}    // namespace bpr::roundtrip
