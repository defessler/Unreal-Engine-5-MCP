// Tests for CachingBlueprintReader. We use a counting in-memory backend
// so we can assert exactly how many times the wrapped reader was called.

#include <doctest/doctest.h>

#include "backends/CachingBlueprintReader.h"
#include "backends/MockBlueprintReader.h"

#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <thread>

using namespace bpr;
using namespace bpr::backends;
using namespace std::chrono_literals;

namespace test_caching_reader_detail {

// Decorator counting forwarded calls. Wraps a real MockBlueprintReader so
// the data shape is canonical.
class CountingReader : public IBlueprintReader {
public:
	explicit CountingReader(MockBlueprintReader inner)
		: inner_(std::move(inner)) {}

	std::atomic<int> readBlueprintCalls{0};
	std::atomic<int> listBlueprintsCalls{0};
	std::atomic<int> listVariablesCalls{0};
	std::atomic<int> getGraphCalls{0};
	std::atomic<int> getFunctionCalls{0};
	std::atomic<int> getComponentsCalls{0};
	std::atomic<int> findNodeCalls{0};

	std::vector<BPAssetSummary> ListBlueprints(std::string_view path) override {
		++listBlueprintsCalls; return inner_.ListBlueprints(path);
	}
	BPMetadata ReadBlueprint(std::string_view a) override {
		++readBlueprintCalls; return inner_.ReadBlueprint(a);
	}
	BPGraph GetGraph(std::string_view a, std::string_view g) override {
		++getGraphCalls; return inner_.GetGraph(a, g);
	}
	BPFunction GetFunction(std::string_view a, std::string_view f) override {
		++getFunctionCalls; return inner_.GetFunction(a, f);
	}
	std::vector<BPVariable> ListVariables(std::string_view a) override {
		++listVariablesCalls; return inner_.ListVariables(a);
	}
	std::vector<BPComponent> GetComponents(std::string_view a) override {
		++getComponentsCalls; return inner_.GetComponents(a);
	}
	std::vector<BPNode> FindNode(std::string_view a, std::string_view q,
								 std::string_view k) override {
		++findNodeCalls; return inner_.FindNode(a, q, k);
	}

	// Write tools — mock throws, but cache layer still invalidates so we
	// forward and let it throw.
	void AddVariable(std::string_view a, std::string_view n, const BPPinType& t,
					 std::string_view d, std::string_view c, bool r, bool e) override {
		inner_.AddVariable(a, n, t, d, c, r, e);
	}
	void SetNodePosition(std::string_view a, std::string_view g, std::string_view n,
						 int x, int y) override {
		inner_.SetNodePosition(a, g, n, x, y);
	}
	void DeleteNode(std::string_view a, std::string_view g, std::string_view n) override {
		inner_.DeleteNode(a, g, n);
	}
	std::string AddNode(std::string_view a, std::string_view g, std::string_view k,
						int x, int y, const std::map<std::string, std::string, std::less<>>& e) override {
		return inner_.AddNode(a, g, k, x, y, e);
	}
	void WirePins(std::string_view a, std::string_view g,
				  std::string_view fn, std::string_view fp,
				  std::string_view tn, std::string_view tp) override {
		inner_.WirePins(a, g, fn, fp, tn, tp);
	}
	void DeleteVariable(std::string_view a, std::string_view n) override {
		inner_.DeleteVariable(a, n);
	}
	void RenameVariable(std::string_view a, std::string_view o, std::string_view n) override {
		inner_.RenameVariable(a, o, n);
	}
	AddFunctionResult AddFunction(std::string_view a, std::string_view n) override {
		return inner_.AddFunction(a, n);
	}
	void AddFunctionInput(std::string_view a, std::string_view f, std::string_view p,
						  const BPPinType& t) override {
		inner_.AddFunctionInput(a, f, p, t);
	}
	void AddFunctionOutput(std::string_view a, std::string_view f, std::string_view p,
						   const BPPinType& t) override {
		inner_.AddFunctionOutput(a, f, p, t);
	}
	void DeleteFunction(std::string_view a, std::string_view n) override {
		inner_.DeleteFunction(a, n);
	}
	void SetVariableDefault(std::string_view a, std::string_view n,
							std::string_view d) override {
		inner_.SetVariableDefault(a, n, d);
	}
	CreateBlueprintResult CreateBlueprint(std::string_view a, std::string_view p) override {
		return inner_.CreateBlueprint(a, p);
	}
	void SetPinDefault(std::string_view a, std::string_view g, std::string_view n,
					   std::string_view pin, std::string_view v) override {
		inner_.SetPinDefault(a, g, n, pin, v);
	}
	void RetypeVariable(std::string_view a, std::string_view n, const BPPinType& t) override {
		inner_.RetypeVariable(a, n, t);
	}
	void SetVariableCategory(std::string_view a, std::string_view n, std::string_view c) override {
		inner_.SetVariableCategory(a, n, c);
	}
	DuplicateBlueprintResult DuplicateBlueprint(std::string_view s, std::string_view d) override {
		return inner_.DuplicateBlueprint(s, d);
	}
	WriteGeneratedSourceResult WriteGeneratedSource(std::string_view p, std::string_view c, bool cd) override {
		return inner_.WriteGeneratedSource(p, c, cd);
	}

	std::atomic<int> runPythonCalls{0};
	PythonResult RunPythonScript(std::string_view code) override {
		++runPythonCalls;
		PythonResult r;
		r.ok = true;
		r.commandResult = std::string("ran:") + std::string(code);
		return r;
	}

private:
	MockBlueprintReader inner_;
};

struct Fixture {
	CountingReader* counter = nullptr;
	std::unique_ptr<CachingBlueprintReader> cache;

	Fixture() {
		auto inner = std::make_unique<CountingReader>(
			test::MakeMockReader());
		counter = inner.get();
		cache = std::make_unique<CachingBlueprintReader>(std::move(inner), 30s);
	}
};

}    // namespace test_caching_reader_detail
using namespace test_caching_reader_detail;

TEST_CASE("Cache: RunPythonScript forwards to inner (regression: wrapper threw 'not supported')") {
	Fixture f;
	auto res = f.cache->RunPythonScript("print('hi')");
	CHECK(f.counter->runPythonCalls == 1);
	CHECK(res.ok);
	CHECK(res.commandResult == "ran:print('hi')");
}

TEST_CASE("Cache: second read of same blueprint hits the cache") {
	Fixture f;
	auto a = f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	auto b = f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	CHECK(f.counter->readBlueprintCalls == 1);
	CHECK(a.Name == b.Name);
	CHECK(f.cache->GetStats().hits  == 1);
	CHECK(f.cache->GetStats().misses == 1);
}

TEST_CASE("Cache: different assets are independent keys") {
	Fixture f;
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ReadBlueprint("/Game/Items/BP_Pickup");
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");      // hit
	f.cache->ReadBlueprint("/Game/Items/BP_Pickup");  // hit
	CHECK(f.counter->readBlueprintCalls == 2);
}

TEST_CASE("Cache: different operations on same asset are independent keys") {
	Fixture f;
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ListVariables("/Game/AI/BP_Enemy");
	f.cache->GetGraph("/Game/AI/BP_Enemy", "EventGraph");
	// All three are first-time misses — none should hit the same slot.
	CHECK(f.counter->readBlueprintCalls   == 1);
	CHECK(f.counter->listVariablesCalls   == 1);
	CHECK(f.counter->getGraphCalls        == 1);
}

TEST_CASE("Cache: GetGraph keyed by (asset, graphName)") {
	Fixture f;
	f.cache->GetGraph("/Game/AI/BP_Enemy", "EventGraph");
	f.cache->GetGraph("/Game/AI/BP_Enemy", "EventGraph");
	CHECK(f.counter->getGraphCalls == 1);
}

TEST_CASE("Cache: FindNode keyed by (asset, query, kind)") {
	Fixture f;
	f.cache->FindNode("/Game/AI/BP_Enemy", "Sequence", "");
	f.cache->FindNode("/Game/AI/BP_Enemy", "Sequence", "");
	f.cache->FindNode("/Game/AI/BP_Enemy", "Branch",   "");  // diff query
	f.cache->FindNode("/Game/AI/BP_Enemy", "Sequence", "Sequence");  // diff kind
	CHECK(f.counter->findNodeCalls == 3);
}

TEST_CASE("Cache: write invalidates the affected asset") {
	Fixture f;
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");  // hit
	CHECK(f.counter->readBlueprintCalls == 1);

	// Mock backend's writes throw — but the decorator MUST still
	// invalidate after a successful pass-through. Use the public
	// InvalidateAsset to simulate a "successful write" scenario without
	// depending on a write-capable backend.
	f.cache->InvalidateAsset("/Game/AI/BP_Enemy");

	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");  // miss again
	CHECK(f.counter->readBlueprintCalls == 2);
}

TEST_CASE("Cache: write to one asset doesn't drop other assets") {
	Fixture f;
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ReadBlueprint("/Game/Items/BP_Pickup");
	f.cache->InvalidateAsset("/Game/AI/BP_Enemy");

	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");      // miss (was invalidated)
	f.cache->ReadBlueprint("/Game/Items/BP_Pickup");  // hit (untouched)
	CHECK(f.counter->readBlueprintCalls == 3);
}

TEST_CASE("Cache: write also invalidates ListBlueprints (modified_iso changes)") {
	Fixture f;
	f.cache->ListBlueprints("/Game");
	f.cache->ListBlueprints("/Game");  // hit
	CHECK(f.counter->listBlueprintsCalls == 1);

	f.cache->InvalidateAsset("/Game/AI/BP_Enemy");

	f.cache->ListBlueprints("/Game");  // miss — list was dropped
	CHECK(f.counter->listBlueprintsCalls == 2);
}

TEST_CASE("Cache: TTL expiry forces a refetch") {
	auto inner = std::make_unique<CountingReader>(
		test::MakeMockReader());
	auto* counter = inner.get();
	CachingBlueprintReader cache(std::move(inner), 100ms);

	cache.ReadBlueprint("/Game/AI/BP_Enemy");
	cache.ReadBlueprint("/Game/AI/BP_Enemy");  // hit
	CHECK(counter->readBlueprintCalls == 1);

	std::this_thread::sleep_for(150ms);

	cache.ReadBlueprint("/Game/AI/BP_Enemy");  // expired -> miss
	CHECK(counter->readBlueprintCalls == 2);
}

TEST_CASE("Cache: InvalidateAll drops everything") {
	Fixture f;
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ListVariables("/Game/Items/BP_Pickup");
	f.cache->ListBlueprints("/Game");
	f.cache->InvalidateAll();
	f.cache->ReadBlueprint("/Game/AI/BP_Enemy");
	f.cache->ListVariables("/Game/Items/BP_Pickup");
	f.cache->ListBlueprints("/Game");
	CHECK(f.counter->readBlueprintCalls   == 2);
	CHECK(f.counter->listVariablesCalls   == 2);
	CHECK(f.counter->listBlueprintsCalls  == 2);
}

TEST_CASE("WrapWithCache(ttl=0) returns the inner reader unwrapped") {
	auto inner = test::MakeMockReaderUnique();
	auto* rawInner = inner.get();
	auto wrapped = WrapWithCache(std::move(inner), 0s);
	// Pointer identity check — the wrapper should pass through.
	CHECK(wrapped.get() == rawInner);
}

TEST_CASE("WrapWithCache(ttl>0) returns a CachingBlueprintReader") {
	auto inner = test::MakeMockReaderUnique();
	auto wrapped = WrapWithCache(std::move(inner), 30s);
	auto* asCache = dynamic_cast<CachingBlueprintReader*>(wrapped.get());
	CHECK(asCache != nullptr);
}

// ===== C2: mtime-based invalidation ========================================

TEST_CASE("Cache C2: external mtime bump evicts a cached entry") {
	namespace fs = std::filesystem;
	// Stage a fake project layout under a temp dir:
	//   <tmp>/MyProj.uproject (placeholder, never read)
	//   <tmp>/Content/AI/BP_Enemy.uasset (matches the mock fixture's
	//                                     /Game/AI/BP_Enemy asset path)
	auto tmpRoot = fs::temp_directory_path() /
				   ("bpr_cache_c2_" + std::to_string(std::random_device{}()));
	fs::create_directories(tmpRoot / "Content" / "AI");
	fs::path uproject = tmpRoot / "MyProj.uproject";
	{ std::ofstream(uproject) << "{}"; }
	fs::path uasset = tmpRoot / "Content" / "AI" / "BP_Enemy.uasset";
	{ std::ofstream(uasset) << "v1"; }

	auto inner = std::make_unique<CountingReader>(
		test::MakeMockReader());
	auto* counter = inner.get();
	CachingBlueprintReader cache(std::move(inner), 30s, uproject);

	// First read primes the cache; mtime captured.
	cache.ReadBlueprint("/Game/AI/BP_Enemy");
	cache.ReadBlueprint("/Game/AI/BP_Enemy");  // hit
	CHECK(counter->readBlueprintCalls == 1);

	// Bump the mtime explicitly so the comparison sees a delta.
	std::this_thread::sleep_for(15ms);
	auto bumped = fs::last_write_time(uasset) + 1s;
	fs::last_write_time(uasset, bumped);

	cache.ReadBlueprint("/Game/AI/BP_Enemy");  // miss — evicted
	CHECK(counter->readBlueprintCalls == 2);

	std::error_code ec;
	fs::remove_all(tmpRoot, ec);
}

TEST_CASE("Cache C2: empty projectDir disables mtime checking") {
	auto inner = std::make_unique<CountingReader>(
		test::MakeMockReader());
	auto* counter = inner.get();
	// Empty projectDir means mtime stamping is skipped — entries serve
	// strictly by TTL like before.
	CachingBlueprintReader cache(std::move(inner), 30s, std::filesystem::path{});
	cache.ReadBlueprint("/Game/AI/BP_Enemy");
	cache.ReadBlueprint("/Game/AI/BP_Enemy");  // hit
	CHECK(counter->readBlueprintCalls == 1);
}
