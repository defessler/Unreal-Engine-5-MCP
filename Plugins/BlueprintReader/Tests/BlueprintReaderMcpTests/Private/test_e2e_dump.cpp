// Diagnostic dumper: writes the transpiled .h / .cpp for the
// BP_ExampleCharacter fixture to <exe-dir>/transpile-dump/ so we can
// eyeball the output. Gated on the test-case name so it runs only when
// explicitly requested -- not part of the default suite.

#include <doctest/doctest.h>

#include "tools/Decompile.h"
#include "tools/codegen/CppClassEmit.h"
#include "backends/MockBlueprintReader.h"

#include "test_helpers.h"

#include <filesystem>
#include <fstream>

using namespace bpr;
using namespace bpr::tools;
using namespace bpr::backends;

TEST_CASE("dump: write BP_ExampleCharacter transpile to disk for inspection") {
	MockBlueprintReader reader(test::FixturesDir());
	auto bpir = DecompileBlueprint(reader, "/Game/Characters/BP_ExampleCharacter");
	auto out  = EmitCppClass(bpir);

	auto outDir = test::TestExecutableDir() / "transpile-dump";
	std::filesystem::create_directories(outDir);

	auto headerPath = outDir / out.headerFileName;
	auto implPath   = outDir / out.implFileName;
	{
		std::ofstream f(headerPath);
		f << out.headerSource;
	}
	{
		std::ofstream f(implPath);
		f << out.implSource;
	}
	// Sanity: both files written + non-empty.
	CHECK(std::filesystem::file_size(headerPath) > 1000);
	CHECK(std::filesystem::file_size(implPath)   > 1000);
}
