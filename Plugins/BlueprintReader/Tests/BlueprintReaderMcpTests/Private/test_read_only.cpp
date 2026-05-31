// Tests for ReadOnlyBlueprintReader — the decorator that gates writes
// when BP_READER_READ_ONLY=1 is set (coexistence with an open editor).

#include <doctest/doctest.h>

#include "backends/BackendFactory.h"
#include "backends/MockBlueprintReader.h"
#include "backends/ReadOnlyBlueprintReader.h"

#include "test_helpers.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

using namespace bpr;
using namespace bpr::backends;

namespace test_read_only_detail {

std::unique_ptr<IBlueprintReader> MakeReadOnly() {
	auto inner = test::MakeMockReaderUnique();
	return std::make_unique<ReadOnlyBlueprintReader>(std::move(inner));
}

// RAII env-var override that restores the prior value (or unsets) on scope
// exit, so these process-global tweaks don't leak across test cases. Pass
// an empty value to unset for the duration. _dupenv_s/_putenv_s on MSVC;
// getenv/setenv elsewhere — mirrors Env.cpp's platform split.
struct ScopedEnv {
	std::string                key_;
	std::optional<std::string> prev_;
	ScopedEnv(const char* key, const char* value) : key_(key) {
#ifdef _MSC_VER
		char*       buf = nullptr;
		std::size_t len = 0;
		if (_dupenv_s(&buf, &len, key) == 0 && buf != nullptr) {
			prev_ = buf;
			std::free(buf);
		}
		_putenv_s(key, value ? value : "");
#else
		if (const char* p = std::getenv(key)) {
			prev_ = p;
		}
		if (value && *value) {
			setenv(key, value, 1);
		} else {
			unsetenv(key);
		}
#endif
	}
	~ScopedEnv() {
#ifdef _MSC_VER
		_putenv_s(key_.c_str(), prev_ ? prev_->c_str() : "");
#else
		if (prev_) {
			setenv(key_.c_str(), prev_->c_str(), 1);
		} else {
			unsetenv(key_.c_str());
		}
#endif
	}
};

}    // namespace test_read_only_detail
using namespace test_read_only_detail;

TEST_CASE("ReadOnly: read tools pass through unchanged") {
	auto r = MakeReadOnly();
	auto list = r->ListBlueprints("/Game");
	CHECK(list.size() >= 2);

	auto meta = r->ReadBlueprint("/Game/AI/BP_Enemy");
	CHECK(meta.Name == "BP_Enemy");

	auto vars = r->ListVariables("/Game/AI/BP_Enemy");
	CHECK(vars.size() >= 1);

	auto graph = r->GetGraph("/Game/AI/BP_Enemy", "EventGraph");
	CHECK(graph.Name == "EventGraph");
}

TEST_CASE("ReadOnly: every write tool throws BlueprintReaderError mentioning the env var") {
	auto r = MakeReadOnly();
	auto check = [](auto&& fn) {
		try {
			fn();
			FAIL("expected throw");
		} catch (const BlueprintReaderError& e) {
			std::string msg = e.what();
			// Message names both the read-only switch and the opt-in so the
			// agent learns how to enable writes from the error alone.
			CHECK(msg.find("BP_READER_READ_ONLY") != std::string::npos);
			CHECK(msg.find("BP_READER_ALLOW_WRITE") != std::string::npos);
		} catch (...) {
			FAIL("expected BlueprintReaderError, got something else");
		}
	};

	BPPinType floatType; floatType.Category = "real"; floatType.SubCategory = "float";

	check([&]{ r->AddVariable("/x","V",floatType,"","",false,false); });
	check([&]{ r->SetNodePosition("/x","g","n",0,0); });
	check([&]{ r->DeleteNode("/x","g","n"); });
	check([&]{ r->AddNode("/x","g","Branch",0,0, {}); });
	check([&]{ r->WirePins("/x","g","n1","p1","n2","p2"); });
	check([&]{ r->DeleteVariable("/x","V"); });
	check([&]{ r->RenameVariable("/x","old","new"); });
	check([&]{ r->AddFunction("/x","Fn"); });
	check([&]{ r->AddFunctionInput("/x","Fn","p",floatType); });
	check([&]{ r->AddFunctionOutput("/x","Fn","p",floatType); });
	check([&]{ r->DeleteFunction("/x","Fn"); });
	check([&]{ r->SetVariableDefault("/x","V","100"); });
	check([&]{ r->CreateBlueprint("/Game/X","Actor"); });
	check([&]{ r->SetPinDefault("/x","g","n","p","v"); });
	check([&]{ r->RetypeVariable("/x","V",floatType); });
	check([&]{ r->SetVariableCategory("/x","V","Cat"); });
	check([&]{ r->DuplicateBlueprint("/Game/X","/Game/Y"); });
	check([&]{ r->RunPythonScript("print('x')"); });
}

TEST_CASE("ReadOnly: error message mentions the affected op name") {
	auto r = MakeReadOnly();
	BPPinType t; t.Category = "bool";
	try {
		r->AddVariable("/x","V",t,"","",false,false);
		FAIL("expected throw");
	} catch (const BlueprintReaderError& e) {
		std::string msg = e.what();
		// Each op identifies itself in the message so multiple violations
		// in a row don't all look identical to the agent.
		CHECK(msg.find("add_variable") != std::string::npos);
	}
}

TEST_CASE("ReadOnly: BeginBatch/EndBatch pass through (apply_ops with all-read ops still works)") {
	auto r = MakeReadOnly();
	// Should not throw — these are pass-through. apply_ops opens a batch
	// before dispatching even if every op is a read.
	r->BeginBatch();
	auto ack = r->EndBatch();
	CHECK(ack.is_object());
}

TEST_CASE("ConfigFromEnv: read-only is the default; ALLOW_WRITE / READ_ONLY=0 opt in") {
	std::ostringstream    log;
	// A path with no .uproject anywhere above it -> no project discovery,
	// no engine/registry lookups; readOnly is computed before any of that.
	std::filesystem::path noProject = "C:/__bpr_no_uproject_here__/Binaries/Win64";

	SUBCASE("nothing set -> read-only (the safe default)") {
		ScopedEnv allow("BP_READER_ALLOW_WRITE", "");  // ensure unset
		ScopedEnv ro("BP_READER_READ_ONLY", "");       // ensure unset
		auto      cfg = ConfigFromEnv(noProject, log);
		CHECK(cfg.readOnly == true);
	}
	SUBCASE("BP_READER_ALLOW_WRITE=1 -> writes enabled") {
		ScopedEnv allow("BP_READER_ALLOW_WRITE", "1");
		ScopedEnv ro("BP_READER_READ_ONLY", "");
		auto      cfg = ConfigFromEnv(noProject, log);
		CHECK(cfg.readOnly == false);
	}
	SUBCASE("BP_READER_READ_ONLY=0 -> writes enabled") {
		ScopedEnv ro("BP_READER_READ_ONLY", "0");
		auto      cfg = ConfigFromEnv(noProject, log);
		CHECK(cfg.readOnly == false);
	}
	SUBCASE("explicit BP_READER_READ_ONLY=1 wins over ALLOW_WRITE=1") {
		ScopedEnv allow("BP_READER_ALLOW_WRITE", "1");
		ScopedEnv ro("BP_READER_READ_ONLY", "1");
		auto      cfg = ConfigFromEnv(noProject, log);
		CHECK(cfg.readOnly == true);
	}
}

TEST_CASE("MaybeWrapReadOnly: false returns the inner unchanged") {
	auto inner = test::MakeMockReaderUnique();
	auto* rawInner = inner.get();
	auto wrapped = MaybeWrapReadOnly(std::move(inner), /*readOnly=*/false);
	CHECK(wrapped.get() == rawInner);
}

TEST_CASE("MaybeWrapReadOnly: true returns a ReadOnly wrapper") {
	auto inner = test::MakeMockReaderUnique();
	auto wrapped = MaybeWrapReadOnly(std::move(inner), /*readOnly=*/true);
	auto* asReadOnly = dynamic_cast<ReadOnlyBlueprintReader*>(wrapped.get());
	CHECK(asReadOnly != nullptr);
}
