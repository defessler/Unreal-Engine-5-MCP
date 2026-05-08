// Tests for the unsupported-node treatment table + sidecar emission.
// Phase 2B closes the loop on "what happens when a BP construct can't
// map cleanly to compilable C++" — every entry has a recipe, every
// encounter logs a structured sidecar entry.

#include <doctest/doctest.h>

#include "tools/codegen/UnsupportedTreatment.h"

using namespace bpr::tools;
using nlohmann::json;

namespace {
bool Contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}
} // namespace

// ===== Classification ======================================================

TEST_CASE("Treatment: K2Node_Timeline → todo_comment with explicit guidance") {
    auto cls = ClassifyUnsupported(json{
        {"node_class", "K2Node_Timeline"}, {"guid", "abc-123"}});
    CHECK(cls.kind == UnsupportedClassification::Kind::TodoComment);
    CHECK(cls.snippet.empty());
    CHECK(Contains(cls.note, "Timeline"));
    CHECK(Contains(cls.note, "UTimelineComponent"));
}

TEST_CASE("Treatment: SpawnActorFromClass → approximation with stub snippet") {
    auto cls = ClassifyUnsupported(json{
        {"node_class", "K2Node_SpawnActorFromClass"}, {"guid", "spawn-1"}});
    CHECK(cls.kind == UnsupportedClassification::Kind::Approximation);
    CHECK(Contains(cls.snippet, "GetWorld()->SpawnActor"));
    // GUID gets substituted into the snippet.
    CHECK(Contains(cls.snippet, "spawn-1"));
    CHECK(Contains(cls.note, "FActorSpawnParameters"));
}

TEST_CASE("Treatment: AnimNode_* uses prefix substring match (one entry covers many)") {
    auto cls1 = ClassifyUnsupported(json{
        {"node_class", "K2Node_AnimNode_BlendListByEnum"}});
    auto cls2 = ClassifyUnsupported(json{
        {"node_class", "K2Node_AnimNode_TransitionPoseEvaluator"}});
    CHECK(cls1.kind == UnsupportedClassification::Kind::TodoComment);
    CHECK(cls2.kind == UnsupportedClassification::Kind::TodoComment);
    CHECK(Contains(cls1.note, "AnimGraph"));
    CHECK(Contains(cls2.note, "AnimGraph"));
}

TEST_CASE("Treatment: AddComponent → approximation with NewObject stub") {
    auto cls = ClassifyUnsupported(json{
        {"node_class", "K2Node_AddComponent"}, {"guid","comp-1"}});
    CHECK(cls.kind == UnsupportedClassification::Kind::Approximation);
    CHECK(Contains(cls.snippet, "NewObject"));
    CHECK(Contains(cls.snippet, "RegisterComponent"));
}

TEST_CASE("Treatment: unknown class falls back to generic todo + helpful note") {
    auto cls = ClassifyUnsupported(json{
        {"node_class", "K2Node_TotallyUnknownXyz"}});
    CHECK(cls.kind == UnsupportedClassification::Kind::TodoComment);
    CHECK(Contains(cls.note, "Unrecognized"));
    CHECK(Contains(cls.note, "K2Node_TotallyUnknownXyz"));
}

TEST_CASE("Treatment: empty / non-object input returns default todo") {
    auto cls = ClassifyUnsupported(json::object());
    CHECK(cls.kind == UnsupportedClassification::Kind::TodoComment);

    auto cls2 = ClassifyUnsupported(json("not an object"));
    CHECK(cls2.kind == UnsupportedClassification::Kind::TodoComment);
}

// ===== Sidecar JSON =======================================================

TEST_CASE("BuildSidecar: empty notes still produces a valid sidecar shell") {
    auto sidecar = BuildSidecar("/Game/AI/BP_Enemy",
                                {"BP_Enemy_Generated.h", "BP_Enemy_Generated.cpp"},
                                json::array());
    CHECK(sidecar["version"] == 1);
    CHECK(sidecar["source_bp"] == "/Game/AI/BP_Enemy");
    REQUIRE(sidecar["generated_files"].is_array());
    CHECK(sidecar["generated_files"].size() == 2);
    CHECK(sidecar["unsupported_nodes"].is_array());
    CHECK(sidecar["approximations"].is_array());
    CHECK(sidecar["unsupported_nodes"].empty());
    CHECK(sidecar["approximations"].empty());
    // Timestamp is ISO-8601 UTC.
    CHECK(sidecar["generated_at"].is_string());
    CHECK(sidecar["generated_at"].get<std::string>().back() == 'Z');
}

TEST_CASE("BuildSidecar: notes split into unsupported vs. approximations") {
    json notes = json::array({
        json{{"node_class","K2Node_Timeline"},{"guid","tl-1"}},
        json{{"node_class","K2Node_SpawnActorFromClass"},{"guid","sp-1"}},
        json{{"node_class","K2Node_AddComponent"},{"guid","ac-1"}},
        json{{"node_class","K2Node_TotallyUnknown"},{"guid","unk-1"}},
    });
    auto sidecar = BuildSidecar("/Game/AI/BP_X", {"BP_X_Generated.h"}, notes);
    REQUIRE(sidecar["unsupported_nodes"].is_array());
    REQUIRE(sidecar["approximations"].is_array());
    // Timeline + Unknown → todo; SpawnActor + AddComponent → approximations.
    CHECK(sidecar["unsupported_nodes"].size() == 2);
    CHECK(sidecar["approximations"].size()    == 2);
}

TEST_CASE("BuildSidecar: each entry carries treatment_kind + manual_steps") {
    json notes = json::array({
        json{{"node_class","K2Node_Timeline"},{"guid","tl-1"}},
    });
    auto sidecar = BuildSidecar("/Game/X", {"X.h"}, notes);
    REQUIRE(sidecar["unsupported_nodes"].size() == 1);
    auto& entry = sidecar["unsupported_nodes"][0];
    CHECK(entry["treatment_kind"] == "todo_comment");
    REQUIRE(entry.contains("manual_steps"));
    CHECK(entry["manual_steps"].is_array());
    CHECK(entry["manual_steps"].size() == 1);
}

TEST_CASE("BuildSidecar: malformed entries don't crash") {
    json notes = json::array({
        "not an object",
        json::array({1, 2, 3}),
        json::object(),
    });
    CHECK_NOTHROW(BuildSidecar("/Game/X", {"X.h"}, notes));
}
