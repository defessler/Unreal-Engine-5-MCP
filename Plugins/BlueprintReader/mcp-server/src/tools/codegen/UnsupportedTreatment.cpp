#include "tools/codegen/UnsupportedTreatment.h"

#include <fmt/core.h>

#include <chrono>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace bpr::tools {

namespace {

// Treatment table — each entry maps a K2 node class to a recipe.
// `match` is a substring (so K2Node_AnimNode_Foo, K2Node_AnimNode_Bar
// can share one entry by matching "K2Node_AnimNode_").
struct Treatment {
    std::string match;
    UnsupportedClassification::Kind kind;
    // Snippet template. `{guid}` and `{class}` are substituted in.
    std::string snippetTemplate;
    std::string note;
};

const std::vector<Treatment>& Table() {
    static const std::vector<Treatment> t = {
        // ----- Timelines -----
        // No clean function-body C++ equivalent — timelines are
        // stateful UTimelineComponent instances on the actor. Future
        // work could plumb this into the constructor codegen; today
        // we surface a TODO + note.
        {"K2Node_Timeline",
         UnsupportedClassification::Kind::TodoComment,
         "",
         "Timeline node found. Manually add a UTimelineComponent member "
         "to the class, configure its UCurveFloat tracks, and call "
         "PlayFromStart() at this point in the function body."},

        // ----- SpawnActor variants -----
        // Best-effort: emit a SpawnActor<> call. The agent verifies
        // SpawnParameters semantics + pin defaults before/after.
        {"K2Node_SpawnActorFromClass",
         UnsupportedClassification::Kind::Approximation,
         "// TODO[bpr-spawn]: verify SpawnParameters from BP node {guid}.\n"
         "// AActor* Spawned = GetWorld()->SpawnActor<AActor>(/*Class=*/nullptr, "
         "FTransform::Identity);\n",
         "BP SpawnActorFromClass uses 'pre-spawn' pin defaults that aren't "
         "directly portable. Translate the input pins (Class, Transform, Owner, "
         "Instigator) into FActorSpawnParameters before calling SpawnActor<>(); "
         "5.x deprecated bNoCollisionFail in favor of SpawnCollisionHandlingOverride."},

        // ----- Latent actions / async -----
        {"K2Node_LatentAbilityCall",
         UnsupportedClassification::Kind::TodoComment,
         "",
         "Latent ability call. The 'Completed' / 'Cancelled' exec outputs "
         "in the BP need manual delegate-binding code on the C++ side."},

        {"K2Node_AsyncAction",
         UnsupportedClassification::Kind::TodoComment,
         "",
         "Async action. UAsyncAction*::CreateXxx returns the action object; "
         "the BP's named exec outputs become OnXxx delegates that need "
         "manual UFUNCTION-bound handlers in C++."},

        // ----- Anim graph nodes -----
        {"K2Node_AnimNode_",
         UnsupportedClassification::Kind::TodoComment,
         "",
         "Anim graph node. AnimGraph runs on the AnimInstance subclass, "
         "not in arbitrary BP function bodies. Move the logic to a "
         "UAnimInstance-derived class manually."},

        // ----- Niagara module graph nodes -----
        {"K2Node_NiagaraXxx",
         UnsupportedClassification::Kind::TodoComment,
         "",
         "Niagara module-graph node. Niagara modules are authored in "
         "their own graph editor; not portable to actor-side C++."},

        // ----- AddComponent -----
        {"K2Node_AddComponent",
         UnsupportedClassification::Kind::Approximation,
         "// TODO[bpr-component]: configure the component spawned by node {guid}.\n"
         "// auto* Comp = NewObject<UActorComponent>(this);\n"
         "// Comp->RegisterComponent();\n",
         "BP AddComponent uses the SCS hierarchy. Translate to NewObject + "
         "RegisterComponent + AttachToComponent. Move SCS subobjects (e.g. "
         "static-mesh children created in the BP class defaults panel) into "
         "the constructor's CreateDefaultSubobject calls."},
    };
    return t;
}

const Treatment* FindTreatment(std::string_view nodeClass) {
    for (const auto& t : Table()) {
        if (nodeClass.find(t.match) != std::string_view::npos) return &t;
    }
    return nullptr;
}

std::string SubstituteTemplate(const std::string& tmpl,
                               const std::string& guid,
                               const std::string& nodeClass) {
    std::string out = tmpl;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(out, "{guid}",  guid);
    replaceAll(out, "{class}", nodeClass);
    return out;
}

std::string IsoTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

UnsupportedClassification ClassifyUnsupported(const nlohmann::json& u) {
    UnsupportedClassification out;
    if (!u.is_object()) return out;
    std::string nodeClass = u.value("node_class", "");
    std::string guid      = u.value("guid", "");
    if (const Treatment* t = FindTreatment(nodeClass)) {
        out.kind = t->kind;
        out.note = t->note;
        if (out.kind == UnsupportedClassification::Kind::Approximation) {
            out.snippet = SubstituteTemplate(t->snippetTemplate, guid, nodeClass);
        }
    } else {
        // Default treatment — generic TODO.
        out.kind = UnsupportedClassification::Kind::TodoComment;
        out.note = fmt::format(
            "Unrecognized BP node class '{}'. Manual port required; "
            "if this is a common pattern, add an entry to "
            "UnsupportedTreatment.cpp's table.", nodeClass);
    }
    return out;
}

nlohmann::json BuildSidecar(std::string_view sourceBp,
                            const std::vector<std::string>& generatedFiles,
                            const nlohmann::json& notes) {
    nlohmann::json unsupportedList = nlohmann::json::array();
    nlohmann::json approximationsList = nlohmann::json::array();

    if (notes.is_array()) {
        for (const auto& n : notes) {
            if (!n.is_object()) continue;
            std::string nodeClass = n.value("node_class", "");
            auto cls = ClassifyUnsupported(n);
            nlohmann::json entry = n;
            // Promote to a richer entry shape: enrich with the
            // treatment metadata so the sidecar is self-describing.
            entry["treatment_kind"] =
                cls.kind == UnsupportedClassification::Kind::Approximation
                    ? "approximation" : "todo_comment";
            if (!cls.note.empty()) entry["manual_steps"] = nlohmann::json::array({cls.note});
            if (cls.kind == UnsupportedClassification::Kind::Approximation) {
                approximationsList.push_back(entry);
            } else {
                unsupportedList.push_back(entry);
            }
        }
    }

    nlohmann::json fileNames = nlohmann::json::array();
    for (const auto& f : generatedFiles) fileNames.push_back(f);

    return nlohmann::json{
        {"version",            1},
        {"generated_at",       IsoTimestamp()},
        {"source_bp",          std::string(sourceBp)},
        {"generated_files",    std::move(fileNames)},
        {"unsupported_nodes",  std::move(unsupportedList)},
        {"approximations",     std::move(approximationsList)},
    };
}

} // namespace bpr::tools
