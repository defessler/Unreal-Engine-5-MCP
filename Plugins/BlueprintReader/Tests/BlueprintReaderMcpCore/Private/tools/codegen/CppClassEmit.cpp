#include "tools/codegen/CppClassEmit.h"
#include "tools/Bpir.h"

#include <fmt/core.h>

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace bpr::tools {

namespace {

// ----- Parent class → header path table ---------------------------------
// UE doesn't ship a programmatic "give me the header for this UClass"
// API at the C++ level — the canonical mapping is convention-driven.
// This covers the BP parent classes that real projects extend in
// practice. Unknowns fall back to a generic UObject include with a
// note so the user can correct the path.
const std::map<std::string, std::string>& ParentHeaderMap() {
    static const std::map<std::string, std::string> m = {
        // Engine — actor lineage.
        {"Object",            "UObject/Object.h"},
        {"UObject",           "UObject/Object.h"},
        {"Actor",             "GameFramework/Actor.h"},
        {"AActor",            "GameFramework/Actor.h"},
        {"Pawn",              "GameFramework/Pawn.h"},
        {"APawn",             "GameFramework/Pawn.h"},
        {"DefaultPawn",       "GameFramework/DefaultPawn.h"},
        {"ADefaultPawn",      "GameFramework/DefaultPawn.h"},
        {"SpectatorPawn",     "GameFramework/SpectatorPawn.h"},
        {"ASpectatorPawn",    "GameFramework/SpectatorPawn.h"},
        {"Character",         "GameFramework/Character.h"},
        {"ACharacter",        "GameFramework/Character.h"},
        {"Controller",        "GameFramework/Controller.h"},
        {"AController",       "GameFramework/Controller.h"},
        {"AIController",      "AIController.h"},
        {"AAIController",     "AIController.h"},
        {"PlayerController",  "GameFramework/PlayerController.h"},
        {"APlayerController", "GameFramework/PlayerController.h"},
        {"PlayerState",       "GameFramework/PlayerState.h"},
        {"APlayerState",      "GameFramework/PlayerState.h"},
        {"PlayerCameraManager",  "Camera/PlayerCameraManager.h"},
        {"APlayerCameraManager", "Camera/PlayerCameraManager.h"},
        {"GameMode",          "GameFramework/GameMode.h"},
        {"AGameMode",         "GameFramework/GameMode.h"},
        {"GameModeBase",      "GameFramework/GameModeBase.h"},
        {"AGameModeBase",     "GameFramework/GameModeBase.h"},
        {"GameState",         "GameFramework/GameState.h"},
        {"AGameState",        "GameFramework/GameState.h"},
        {"GameStateBase",     "GameFramework/GameStateBase.h"},
        {"AGameStateBase",    "GameFramework/GameStateBase.h"},
        {"HUD",               "GameFramework/HUD.h"},
        {"AHUD",              "GameFramework/HUD.h"},
        {"Info",              "GameFramework/Info.h"},
        {"AInfo",             "GameFramework/Info.h"},
        {"WorldSettings",     "GameFramework/WorldSettings.h"},
        {"AWorldSettings",    "GameFramework/WorldSettings.h"},
        {"StaticMeshActor",   "Engine/StaticMeshActor.h"},
        {"AStaticMeshActor",  "Engine/StaticMeshActor.h"},
        // Volumes.
        {"Volume",            "GameFramework/Volume.h"},
        {"AVolume",           "GameFramework/Volume.h"},
        {"BlockingVolume",    "Engine/BlockingVolume.h"},
        {"ABlockingVolume",   "Engine/BlockingVolume.h"},
        {"TriggerBox",        "Engine/TriggerBox.h"},
        {"ATriggerBox",       "Engine/TriggerBox.h"},
        {"TriggerSphere",     "Engine/TriggerSphere.h"},
        {"ATriggerSphere",    "Engine/TriggerSphere.h"},
        {"TriggerCapsule",    "Engine/TriggerCapsule.h"},
        {"ATriggerCapsule",   "Engine/TriggerCapsule.h"},
        {"TriggerVolume",     "Engine/TriggerVolume.h"},
        {"ATriggerVolume",    "Engine/TriggerVolume.h"},
        {"PostProcessVolume", "Engine/PostProcessVolume.h"},
        {"APostProcessVolume","Engine/PostProcessVolume.h"},
        // Lights.
        {"Light",                  "Engine/Light.h"},
        {"ALight",                 "Engine/Light.h"},
        {"DirectionalLight",       "Engine/DirectionalLight.h"},
        {"ADirectionalLight",      "Engine/DirectionalLight.h"},
        {"PointLight",             "Engine/PointLight.h"},
        {"APointLight",            "Engine/PointLight.h"},
        {"SpotLight",              "Engine/SpotLight.h"},
        {"ASpotLight",             "Engine/SpotLight.h"},
        {"RectLight",              "Engine/RectLight.h"},
        {"ARectLight",             "Engine/RectLight.h"},
        {"SkyLight",               "Engine/SkyLight.h"},
        {"ASkyLight",              "Engine/SkyLight.h"},
        // Components — base.
        {"ActorComponent",         "Components/ActorComponent.h"},
        {"UActorComponent",        "Components/ActorComponent.h"},
        {"SceneComponent",         "Components/SceneComponent.h"},
        {"USceneComponent",        "Components/SceneComponent.h"},
        {"PrimitiveComponent",     "Components/PrimitiveComponent.h"},
        {"UPrimitiveComponent",    "Components/PrimitiveComponent.h"},
        // Components — meshes.
        {"StaticMeshComponent",                  "Components/StaticMeshComponent.h"},
        {"UStaticMeshComponent",                 "Components/StaticMeshComponent.h"},
        {"InstancedStaticMeshComponent",         "Components/InstancedStaticMeshComponent.h"},
        {"UInstancedStaticMeshComponent",        "Components/InstancedStaticMeshComponent.h"},
        {"HierarchicalInstancedStaticMeshComponent",
                                                 "Components/HierarchicalInstancedStaticMeshComponent.h"},
        {"UHierarchicalInstancedStaticMeshComponent",
                                                 "Components/HierarchicalInstancedStaticMeshComponent.h"},
        {"SkeletalMeshComponent",                "Components/SkeletalMeshComponent.h"},
        {"USkeletalMeshComponent",               "Components/SkeletalMeshComponent.h"},
        {"SkinnedMeshComponent",                 "Components/SkinnedMeshComponent.h"},
        {"USkinnedMeshComponent",                "Components/SkinnedMeshComponent.h"},
        // Components — collision shapes.
        {"BoxComponent",      "Components/BoxComponent.h"},
        {"UBoxComponent",     "Components/BoxComponent.h"},
        {"SphereComponent",   "Components/SphereComponent.h"},
        {"USphereComponent",  "Components/SphereComponent.h"},
        {"CapsuleComponent",  "Components/CapsuleComponent.h"},
        {"UCapsuleComponent", "Components/CapsuleComponent.h"},
        {"ShapeComponent",    "Components/ShapeComponent.h"},
        {"UShapeComponent",   "Components/ShapeComponent.h"},
        // Components — camera + spring arm.
        {"CameraComponent",     "Camera/CameraComponent.h"},
        {"UCameraComponent",    "Camera/CameraComponent.h"},
        {"SpringArmComponent",  "GameFramework/SpringArmComponent.h"},
        {"USpringArmComponent", "GameFramework/SpringArmComponent.h"},
        // Components — movement.
        {"MovementComponent",          "GameFramework/MovementComponent.h"},
        {"UMovementComponent",         "GameFramework/MovementComponent.h"},
        {"PawnMovementComponent",      "GameFramework/PawnMovementComponent.h"},
        {"UPawnMovementComponent",     "GameFramework/PawnMovementComponent.h"},
        {"CharacterMovementComponent", "GameFramework/CharacterMovementComponent.h"},
        {"UCharacterMovementComponent","GameFramework/CharacterMovementComponent.h"},
        {"FloatingPawnMovement",       "GameFramework/FloatingPawnMovement.h"},
        {"UFloatingPawnMovement",      "GameFramework/FloatingPawnMovement.h"},
        {"ProjectileMovementComponent","GameFramework/ProjectileMovementComponent.h"},
        {"UProjectileMovementComponent","GameFramework/ProjectileMovementComponent.h"},
        {"RotatingMovementComponent",  "GameFramework/RotatingMovementComponent.h"},
        {"URotatingMovementComponent", "GameFramework/RotatingMovementComponent.h"},
        // Components — visual / utility.
        {"ParticleSystemComponent",  "Particles/ParticleSystemComponent.h"},
        {"UParticleSystemComponent", "Particles/ParticleSystemComponent.h"},
        {"AudioComponent",   "Components/AudioComponent.h"},
        {"UAudioComponent",  "Components/AudioComponent.h"},
        {"WidgetComponent",  "Components/WidgetComponent.h"},
        {"UWidgetComponent", "Components/WidgetComponent.h"},
        {"BillboardComponent",  "Components/BillboardComponent.h"},
        {"UBillboardComponent", "Components/BillboardComponent.h"},
        {"ArrowComponent",    "Components/ArrowComponent.h"},
        {"UArrowComponent",   "Components/ArrowComponent.h"},
        {"DecalComponent",    "Components/DecalComponent.h"},
        {"UDecalComponent",   "Components/DecalComponent.h"},
        {"ChildActorComponent","Components/ChildActorComponent.h"},
        {"UChildActorComponent","Components/ChildActorComponent.h"},
        {"TimelineComponent", "Components/TimelineComponent.h"},
        {"UTimelineComponent","Components/TimelineComponent.h"},
        {"PostProcessComponent",  "Components/PostProcessComponent.h"},
        {"UPostProcessComponent", "Components/PostProcessComponent.h"},
        // Engine globals.
        {"GameInstance",            "Engine/GameInstance.h"},
        {"UGameInstance",           "Engine/GameInstance.h"},
        {"LocalPlayer",             "Engine/LocalPlayer.h"},
        {"ULocalPlayer",            "Engine/LocalPlayer.h"},
        {"World",                   "Engine/World.h"},
        {"UWorld",                  "Engine/World.h"},
        {"Level",                   "Engine/Level.h"},
        {"ULevel",                  "Engine/Level.h"},
        // Subsystems.
        {"Subsystem",                  "Subsystems/Subsystem.h"},
        {"USubsystem",                 "Subsystems/Subsystem.h"},
        {"GameInstanceSubsystem",      "Subsystems/GameInstanceSubsystem.h"},
        {"UGameInstanceSubsystem",     "Subsystems/GameInstanceSubsystem.h"},
        {"WorldSubsystem",             "Subsystems/WorldSubsystem.h"},
        {"UWorldSubsystem",            "Subsystems/WorldSubsystem.h"},
        {"LocalPlayerSubsystem",       "Subsystems/LocalPlayerSubsystem.h"},
        {"ULocalPlayerSubsystem",      "Subsystems/LocalPlayerSubsystem.h"},
        {"EngineSubsystem",            "Subsystems/EngineSubsystem.h"},
        {"UEngineSubsystem",           "Subsystems/EngineSubsystem.h"},
        // Save / data.
        {"SaveGame",          "GameFramework/SaveGame.h"},
        {"USaveGame",         "GameFramework/SaveGame.h"},
        {"DataAsset",         "Engine/DataAsset.h"},
        {"UDataAsset",        "Engine/DataAsset.h"},
        {"PrimaryDataAsset",  "Engine/DataAsset.h"},
        {"UPrimaryDataAsset", "Engine/DataAsset.h"},
        {"DataTable",         "Engine/DataTable.h"},
        {"UDataTable",        "Engine/DataTable.h"},
        // BP libraries.
        {"BlueprintFunctionLibrary",   "Kismet/BlueprintFunctionLibrary.h"},
        {"UBlueprintFunctionLibrary",  "Kismet/BlueprintFunctionLibrary.h"},
        {"BlueprintAsyncActionBase",   "Blueprint/BlueprintAsyncActionBase.h"},
        {"UBlueprintAsyncActionBase",  "Blueprint/BlueprintAsyncActionBase.h"},
        // UI / Slate / UMG.
        {"UserWidget",        "Blueprint/UserWidget.h"},
        {"UUserWidget",       "Blueprint/UserWidget.h"},
        // Animation.
        {"AnimInstance",      "Animation/AnimInstance.h"},
        {"UAnimInstance",     "Animation/AnimInstance.h"},
        {"AnimNotify",        "Animation/AnimNotifies/AnimNotify.h"},
        {"UAnimNotify",       "Animation/AnimNotifies/AnimNotify.h"},
        {"AnimNotifyState",   "Animation/AnimNotifies/AnimNotifyState.h"},
        {"UAnimNotifyState",  "Animation/AnimNotifies/AnimNotifyState.h"},
    };
    return m;
}

std::string StripClassPrefix(std::string_view name) {
    if (name.size() >= 2 &&
        (name[0] == 'A' || name[0] == 'U') &&
        name[1] >= 'A' && name[1] <= 'Z') {
        return std::string(name.substr(1));
    }
    return std::string(name);
}

// Determine whether a parent class is in the Actor lineage (uses A
// prefix) vs. plain UObject lineage (U prefix). Falls back to U for
// unknowns, which matches UE convention for non-Actor objects.
char ChoosePrefixFor(std::string_view parentClassName) {
    if (parentClassName.empty()) return 'U';
    if (parentClassName[0] == 'A' || parentClassName[0] == 'U') {
        // Already-prefixed: take its prefix.
        return parentClassName[0];
    }
    // Unprefixed parent — use the same heuristic as CppEmit's object
    // mapper. Actor-derived names get A, everything else U.
    if (parentClassName == "Actor" ||
        (parentClassName.size() > 5 &&
         parentClassName.substr(parentClassName.size() - 5) == "Actor") ||
        parentClassName == "Pawn" ||
        parentClassName == "Character" ||
        parentClassName == "Controller" ||
        parentClassName == "PlayerController" ||
        parentClassName == "PlayerState" ||
        parentClassName == "GameMode" ||
        parentClassName == "GameModeBase" ||
        parentClassName == "GameState" ||
        parentClassName == "HUD" ||
        parentClassName == "Info") {
        return 'A';
    }
    return 'U';
}

// Normalize the parent class to its prefixed form.
std::string NormalizeParent(std::string_view parentClassName) {
    if (parentClassName.empty()) return "UObject";
    // Strip "/Script/Engine.Actor" → "Actor".
    auto dot = parentClassName.find_last_of('.');
    std::string_view base = (dot == std::string_view::npos)
                              ? parentClassName
                              : parentClassName.substr(dot + 1);
    if (base.size() >= 2 &&
        (base[0] == 'A' || base[0] == 'U') &&
        base[1] >= 'A' && base[1] <= 'Z') {
        return std::string(base);
    }
    return std::string(1, ChoosePrefixFor(base)) + std::string(base);
}

// Return the "_C" stripped form (BP class names sometimes carry it).
std::string StripBpSuffix(std::string_view bpName) {
    if (bpName.size() > 2 && bpName.substr(bpName.size() - 2) == "_C") {
        return std::string(bpName.substr(0, bpName.size() - 2));
    }
    return std::string(bpName);
}

} // namespace

std::string ParentClassToHeader(std::string_view parentClassName) {
    if (parentClassName.empty()) return "UObject/Object.h";
    // Strip path/dot prefix.
    auto dot = parentClassName.find_last_of('.');
    std::string base = (dot == std::string_view::npos)
                          ? std::string(parentClassName)
                          : std::string(parentClassName.substr(dot + 1));
    auto it = ParentHeaderMap().find(base);
    if (it != ParentHeaderMap().end()) return it->second;
    // Unknown — best-guess as a project-local header. UE convention
    // names headers after the class without the prefix
    // (`AMyEnemy` → `MyEnemy.h`), so strip A/U if present.
    std::string clean = base;
    if (clean.size() >= 2 &&
        (clean[0] == 'A' || clean[0] == 'U') &&
        clean[1] >= 'A' && clean[1] <= 'Z') {
        clean = clean.substr(1);
    }
    return clean + ".h";
}

std::string PrefixClassName(std::string_view bpName, std::string_view parentClass) {
    std::string clean = StripBpSuffix(bpName);
    // If already prefixed (someone passed "ABP_Enemy"), preserve it.
    if (clean.size() >= 2 &&
        (clean[0] == 'A' || clean[0] == 'U') &&
        clean[1] >= 'A' && clean[1] <= 'Z') {
        return clean;
    }
    return std::string(1, ChoosePrefixFor(parentClass)) + clean;
}

// Best-effort: detect whether a BP variable's type is a UE component
// reference. Component UPROPERTYs follow a different convention from
// data UPROPERTYs (`VisibleAnywhere, BlueprintReadOnly` rather than
// `EditAnywhere, BlueprintReadWrite`) because re-assigning the
// component pointer at runtime would orphan the original — but the
// component's inner properties should still be editable via the
// details panel cascade.
//
// Heuristic: bpirType is "object:X" where X is U*Component (after the
// usual path-strip and prefix-detection logic). Returns true for
// UStaticMeshComponent, USceneComponent, UCharacterMovementComponent,
// etc. — anything ending in "Component".
bool LooksLikeComponentType(std::string_view bpirType) {
    // Strip object:/soft_object: prefix.
    std::string_view t = bpirType;
    auto colon = t.find(':');
    if (colon == std::string_view::npos) return false;
    std::string_view head = t.substr(0, colon);
    if (head != "object" && head != "soft_object") return false;
    std::string_view sub = t.substr(colon + 1);
    if (auto dot = sub.find_last_of('.'); dot != std::string_view::npos) {
        sub = sub.substr(dot + 1);
    }
    if (sub.size() > 2 && sub.substr(sub.size() - 2) == "_C") {
        sub = sub.substr(0, sub.size() - 2);
    }
    return sub.size() > 9 &&
           sub.substr(sub.size() - 9) == "Component";
}

std::string BuildUPropertyList(const nlohmann::json& varDecl) {
    std::vector<std::string> specs;
    const std::string typeStr = varDecl.value("type", "");
    const bool isComponent = LooksLikeComponentType(typeStr);

    // Component fields take a different specifier path — see
    // LooksLikeComponentType comment. Data fields take EditAnywhere /
    // BlueprintReadWrite when editable; non-editable data still gets
    // BlueprintReadWrite (matches BP semantics — vars are visible to BP
    // graphs by default).
    if (isComponent) {
        // Always visible in editor + BlueprintReadOnly. UE convention
        // for SCS-attached and CreateDefaultSubobject components alike.
        specs.push_back("VisibleAnywhere");
        specs.push_back("BlueprintReadOnly");
    } else {
        if (varDecl.value("editable", false)) {
            specs.push_back("EditAnywhere");
        }
        // BP variables are always Blueprint-visible.
        specs.push_back("BlueprintReadWrite");
    }
    // Replication: prefer ReplicatedUsing= when a rep-notify callback
    // is set on the BP variable — that's what BP's "RepNotify" toggle
    // turns into at compile time. Falls back to plain Replicated when
    // the variable is replicated but doesn't carry a notify function.
    if (varDecl.value("replicated", false)) {
        std::string notifyFn;
        if (auto it = varDecl.find("rep_notify_func");
            it != varDecl.end() && it->is_string()) {
            notifyFn = it->get<std::string>();
        }
        if (!notifyFn.empty()) {
            specs.push_back(fmt::format("ReplicatedUsing={}", notifyFn));
        } else {
            specs.push_back("Replicated");
        }
    }
    if (auto it = varDecl.find("category"); it != varDecl.end() && it->is_string()
        && !it->get_ref<const std::string&>().empty()) {
        specs.push_back(fmt::format(R"(Category="{}")", it->get<std::string>()));
    }
    // ExposeOnSpawn → UPROPERTY meta=(ExposeOnSpawn="true"). BP's
    // "Expose On Spawn" checkbox surfaces the variable on Spawn-style
    // creation nodes — same meta key works for C++ callers.
    if (varDecl.value("expose_on_spawn", false)) {
        specs.push_back(R"(meta=(ExposeOnSpawn="true"))");
    }
    std::string out;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (i) out += ", ";
        out += specs[i];
    }
    return out;
}

std::string BuildUFunctionList(const nlohmann::json& fnDoc) {
    std::vector<std::string> specs;
    // Default for BP-authored functions: BlueprintCallable. The plan
    // mentions BlueprintImplementableEvent / BlueprintNativeEvent
    // inference from BP metadata; v1 ships the common case and adds
    // explicit specifiers when the metadata carries them.
    bool sawExplicit = false;
    bool isPure = false;
    if (fnDoc.contains("metadata") && fnDoc["metadata"].is_object()) {
        const auto& md = fnDoc["metadata"];
        if (md.contains("ufunction_specifiers") &&
            md["ufunction_specifiers"].is_array()) {
            for (const auto& s : md["ufunction_specifiers"]) {
                if (s.is_string()) {
                    specs.push_back(s.get<std::string>());
                    sawExplicit = true;
                }
            }
        }
        // Pure inference: Decompile sets metadata.pure=true when the
        // BP's FunctionEntry has no exec output AND the function has
        // a return value. Matches UE's BlueprintPure semantics.
        isPure = md.value("pure", false);
    }
    if (!sawExplicit) {
        specs.push_back(isPure ? "BlueprintPure" : "BlueprintCallable");
    }
    std::string out;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (i) out += ", ";
        out += specs[i];
    }
    return out;
}

namespace {

// Trim trailing zeros from a BP-serialized float string. UE serializes
// "100.0" as "100.000000" — we render that as "100.0f" rather than
// "100.000000f" so the generated C++ reads cleanly. Preserves a single
// trailing zero after the decimal point ("0" -> "0.0", not "0.").
std::string TrimFloatDefault(std::string_view in) {
    std::string s(in);
    auto dot = s.find('.');
    if (dot == std::string::npos) return s + ".0";
    // Trim trailing zeros after the decimal point, but keep at least
    // one digit after the dot so we don't end up with bare "100.".
    // We scan from the right while we're past `dot+1` and the char is '0'.
    while (s.size() > dot + 2 && s.back() == '0') s.pop_back();
    return s;
}

// Render a BPIR variable as a UPROPERTY decl + initializer.
//   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Stats", Replicated)
//   float Health = 100.0f;
std::string RenderUPropertyDecl(const nlohmann::json& v) {
    std::string specs = BuildUPropertyList(v);
    std::string typeStr = v.value("type", "void");
    // UPROPERTY-context type: wraps UObject* members with TObjectPtr<>
    // per UE5 convention (Epic recommends since 5.0).
    std::string cppType = MapBpirTypeToCppMember(typeStr);
    std::string name    = v.value("name", "Var");
    std::string defaultStr = v.value("default", std::string{});
    // Safety default for primitive UPROPERTYs: BP defaults bool to
    // false, numerics to 0 — when BP doesn't carry an explicit value,
    // we must still initialize because uninitialized primitive class
    // members are undefined behavior in C++. Pointers / TObjectPtr
    // default-construct to nullptr so they're safe without our help.
    if (defaultStr.empty()) {
        if (cppType == "bool") defaultStr = "false";
        else if (cppType == "int32" || cppType == "int64" || cppType == "uint8" ||
                 cppType == "uint16" || cppType == "uint32" || cppType == "uint64" ||
                 cppType == "int16" || cppType == "int8") {
            defaultStr = "0";
        }
        else if (cppType == "float" || cppType == "double") defaultStr = "0";
    }
    std::string line = fmt::format("    UPROPERTY({})\n    {} {}", specs, cppType, name);
    if (!defaultStr.empty()) {
        // Defensive: we can't parse arbitrary BP default strings to
        // C++ initializers, but for primitives the BP serialization
        // matches C++ literal syntax closely enough (e.g. "100.000000"
        // → "100.0f", "true" → "true", "/Script/..." object refs
        // we punt on).
        if (defaultStr == "true" || defaultStr == "false") {
            line += fmt::format(" = {}", defaultStr);
        } else if (cppType == "float" || cppType == "double") {
            // BP often serializes floats with trailing zeros — trim
            // for readability. Mark with `f` only on float (not double).
            std::string trimmed = TrimFloatDefault(defaultStr);
            line += (cppType == "float")
                ? fmt::format(" = {}f", trimmed)
                : fmt::format(" = {}",  trimmed);
        } else if (cppType.find("int") == 0 || cppType == "uint8") {
            // Strip trailing decimals from BP's "0.000000" form on int fields.
            std::string trimmed = defaultStr;
            auto dot = trimmed.find('.');
            if (dot != std::string::npos) trimmed = trimmed.substr(0, dot);
            line += fmt::format(" = {}", trimmed);
        } else if (cppType == "FString") {
            line += fmt::format(" = TEXT(\"{}\")", defaultStr);
        } else if (cppType == "FName") {
            line += fmt::format(" = TEXT(\"{}\")", defaultStr);
        }
        // Other types: skip the inline default. Constructor body or
        // BeginPlay can set them later.
    }
    line += ";\n";
    return line;
}

// UE base-class methods that are commonly virtual in the lineage —
// emitting our own `UFUNCTION() bool TakeDamage(float)` shadows the
// inherited `virtual float AActor::TakeDamage(float, FDamageEvent, ...)`
// instead of overriding it. We detect these and add a sidecar warning
// so the agent knows to either rename or refactor to the real
// override signature.
const std::unordered_set<std::string>& UeReservedMethodNames() {
    static const std::unordered_set<std::string> s = {
        // AActor
        "TakeDamage", "Tick", "BeginPlay", "EndPlay", "PostInitProperties",
        "PreInitializeComponents", "PostInitializeComponents", "Destroyed",
        "GetActorLocation", "SetActorLocation", "GetActorRotation",
        "SetActorRotation", "GetActorScale3D", "SetActorScale3D",
        // APawn
        "PossessedBy", "UnPossessed", "Restart",
        // ACharacter
        "Jump", "StopJumping", "Landed", "Falling", "OnLanded",
        // UObject
        "PostLoad", "PostInitProperties", "Serialize",
    };
    return s;
}

// Render UFUNCTION decl for the header.
//   UFUNCTION(BlueprintCallable, Category="Combat")
//   void TakeDamage(float Amount, bool& Killed);
std::string RenderUFunctionDecl(const nlohmann::json& fn) {
    std::string specs = BuildUFunctionList(fn);
    std::string returnType = "void";
    if (fn.contains("metadata") && fn["metadata"].is_object()) {
        returnType = fn["metadata"].value("return_type", returnType);
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() == 1) {
        returnType = MapBpirTypeToCpp(fn["outputs"][0].value("type", "void"));
    }
    std::string args;
    if (fn.contains("inputs") && fn["inputs"].is_array()) {
        bool first = true;
        for (const auto& in : fn["inputs"]) {
            if (!first) args += ", ";
            first = false;
            args += MapBpirTypeToCppArg(in.value("type", "void"));
            args += " ";
            args += in.value("name", "Arg");
        }
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() > 1) {
        for (const auto& out : fn["outputs"]) {
            if (!args.empty()) args += ", ";
            args += MapBpirTypeToCppArg(out.value("type", "void"));
            args += "& ";
            args += out.value("name", "Out");
        }
    }
    std::string fnName = fn.value("name", "Fn");
    // BlueprintPure → emit `const` on the member function. UE auto-
    // derives the Pure-ness from const-ness when the function has a
    // return value, so this keeps the two specifiers in lockstep.
    bool isPure = false;
    if (fn.contains("metadata") && fn["metadata"].is_object()) {
        isPure = fn["metadata"].value("pure", false);
    }
    const char* constSuffix = isPure ? " const" : "";
    return fmt::format("    UFUNCTION({})\n    {} {}({}){};\n",
                       specs, returnType, fnName, args, constSuffix);
}

// Detect whether a function carries any of UE's RPC specifiers
// (Server / Client / NetMulticast). The C++ binding pattern for RPCs
// is that the impl is named <FnName>_Implementation -- UE's reflection
// generates the entry-point wrapper. Without the suffix, the linker
// complains about a missing _Implementation symbol when UHT emits its
// generated.cpp glue.
bool IsRpcFunction(const nlohmann::json& fn) {
    if (!fn.contains("metadata") || !fn["metadata"].is_object()) return false;
    const auto& md = fn["metadata"];
    if (!md.contains("ufunction_specifiers") ||
        !md["ufunction_specifiers"].is_array()) return false;
    for (const auto& s : md["ufunction_specifiers"]) {
        if (!s.is_string()) continue;
        const std::string& v = s.get_ref<const std::string&>();
        if (v == "Server" || v == "Client" || v == "NetMulticast") return true;
    }
    return false;
}

// Function-body emission for the .cpp side. Re-uses CppEmit but
// renders as a method definition: ReturnType ClassName::FnName(args) { ... }
std::string RenderUFunctionImpl(const std::string& className,
                                const nlohmann::json& fn,
                                CppEmitOptions emitOpts,
                                nlohmann::json& accumulatedNotes) {
    std::string returnType = "void";
    if (fn.contains("metadata") && fn["metadata"].is_object()) {
        returnType = fn["metadata"].value("return_type", returnType);
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() == 1) {
        returnType = MapBpirTypeToCpp(fn["outputs"][0].value("type", "void"));
    }
    std::string args;
    if (fn.contains("inputs") && fn["inputs"].is_array()) {
        bool first = true;
        for (const auto& in : fn["inputs"]) {
            if (!first) args += ", ";
            first = false;
            args += MapBpirTypeToCppArg(in.value("type", "void"));
            args += " ";
            args += in.value("name", "Arg");
        }
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() > 1) {
        for (const auto& out : fn["outputs"]) {
            if (!args.empty()) args += ", ";
            args += MapBpirTypeToCppArg(out.value("type", "void"));
            args += "& ";
            args += out.value("name", "Out");
        }
    }
    std::string fnName = fn.value("name", "Fn");
    // RPC functions: rename the impl to <FnName>_Implementation. The
    // header decl stays as <FnName> -- UHT generates the dispatch
    // wrapper that calls _Implementation.
    if (IsRpcFunction(fn)) {
        fnName += "_Implementation";
    }

    bool isPure = false;
    if (fn.contains("metadata") && fn["metadata"].is_object()) {
        isPure = fn["metadata"].value("pure", false);
    }
    const char* constSuffix = isPure ? " const" : "";

    auto bodyResult = EmitCppFunctionBody(fn, emitOpts);
    for (const auto& n : bodyResult.notes) accumulatedNotes.push_back(n);

    return fmt::format(
        "{} {}::{}({}){} {{\n{}}}\n",
        returnType, className, fnName, args, constSuffix, bodyResult.source);
}

} // namespace

CppClassEmitResult EmitCppClass(const nlohmann::json& doc,
                                CppClassEmitOptions opts) {
    ValidateBpir(doc);
    if (!IsBpirClass(doc)) {
        throw std::invalid_argument(
            "EmitCppClass requires a BPIR class doc (kind=\"class\")");
    }

    // ----- Resolve names ----------------------------------------------
    std::string bpName       = doc.value("name", "Generated");
    std::string parentRaw    = "UObject";
    if (doc.contains("metadata") && doc["metadata"].is_object()) {
        parentRaw = doc["metadata"].value("parent_class", parentRaw);
    }
    std::string parentClass  = NormalizeParent(parentRaw);
    // Project-prefix injection. PrefixClassName gives us
    // "ABP_Enemy"; if the project passed classNamePrefix="Foo" we
    // want "AFooBPEnemy" (UE letter + house prefix + camel-cased BP
    // name with underscores collapsed). Empty prefix preserves the
    // legacy "ABP_Enemy" form for backward compat.
    std::string baseClassName = PrefixClassName(bpName, parentClass);
    if (!opts.classNamePrefix.empty()) {
        // Strip the single UE type letter (A/U/I) off the front, then
        // collapse underscores out of the BP-name part.
        std::string ueLetter;
        std::string rest = baseClassName;
        if (!rest.empty() && (rest[0] == 'A' || rest[0] == 'U' || rest[0] == 'I')) {
            ueLetter = rest.substr(0, 1);
            rest = rest.substr(1);
        }
        std::string camel;
        bool upNext = true;
        for (char c : rest) {
            if (c == '_') { upNext = true; continue; }
            if (upNext) {
                camel += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                upNext = false;
            } else {
                camel += c;
            }
        }
        baseClassName = ueLetter + opts.classNamePrefix + camel;
    }
    std::string className     = baseClassName + opts.classNameSuffix;
    std::string cleanFileBase = StripBpSuffix(bpName) + opts.classNameSuffix;

    CppClassEmitResult out;
    out.className      = className;
    out.headerFileName = cleanFileBase + ".h";
    out.implFileName   = cleanFileBase + ".cpp";

    // ----- Header generation ------------------------------------------
    std::ostringstream H;
    H << "// AUTO-GENERATED by bp-reader transpile. Do not edit — re-run to regenerate.\n";
    if (doc.contains("metadata") && doc["metadata"].is_object() &&
        doc["metadata"].contains("asset_path")) {
        H << "// Source BP: " << doc["metadata"]["asset_path"].get<std::string>() << "\n";
    }
    H << "// BPIR version: " << kBpirSchemaVersion << "\n\n";
    H << "#pragma once\n\n";
    H << "#include \"CoreMinimal.h\"\n";

    H << "#include \"" << ParentClassToHeader(parentClass) << "\"\n";

    // Resolve component class paths to bare C++ class names. SCS
    // components carry their class as `/Script/Engine.StaticMeshComponent`
    // etc.; we strip to `StaticMeshComponent` then U-prefix it. For BP
    // class component types, strip _C. Used by both forward decls and
    // the UPROPERTY+constructor emission below.
    auto resolveComponentClass = [](std::string_view classPath) -> std::string {
        // Take the last segment after the dot.
        std::string_view bare = classPath;
        if (auto dot = bare.find_last_of('.'); dot != std::string_view::npos) {
            bare = bare.substr(dot + 1);
        }
        // Strip _C suffix (BP class).
        if (bare.size() > 2 && bare.substr(bare.size() - 2) == "_C") {
            bare = bare.substr(0, bare.size() - 2);
        }
        std::string s(bare);
        // U-prefix unless already prefixed. Components are always
        // UObject-derived (UActorComponent etc.), never A-prefixed.
        if (s.size() >= 2 &&
            (s[0] == 'A' || s[0] == 'U') &&
            s[1] >= 'A' && s[1] <= 'Z') {
            return s;
        }
        return "U" + s;
    };

    // Forward declarations for UPROPERTY-referenced object types.
    // UE convention: forward declare in the header, include in the .cpp.
    // Avoids pulling in every Component/Asset header into downstream
    // translation units that just #include this generated header.
    // Skip parent class (already included) and primitives (no decl
    // needed). Component types get a `class` forward decl; we don't
    // emit declarations for types we don't recognize as UObject-derived.
    std::set<std::string> forwardDecls;
    // SCS components also need forward declarations.
    if (doc.contains("components") && doc["components"].is_array()) {
        for (const auto& c : doc["components"]) {
            std::string cls = resolveComponentClass(c.value("class", ""));
            if (!cls.empty() && cls != parentClass) {
                forwardDecls.insert(std::move(cls));
            }
        }
    }
    if (doc.contains("variables") && doc["variables"].is_array()) {
        for (const auto& v : doc["variables"]) {
            std::string typeStr = v.value("type", "");
            // Extract the "X" out of "object:X" / "soft_object:X" / "[]object:X".
            auto colon = typeStr.find(':');
            if (colon == std::string::npos) continue;
            std::string head = typeStr.substr(0, colon);
            // Walk past TArray/TSet container prefixes.
            // "[]object:X" head = "[]object". We want the leaf type only.
            auto isObjectHead = [](std::string_view s) {
                // Strip optional `[]` / `{}` container prefix.
                while (s.size() >= 2 && (s[0] == '[' || s[0] == '{')) s = s.substr(2);
                return s == "object" || s == "soft_object";
            };
            if (!isObjectHead(head)) continue;
            std::string sub = typeStr.substr(colon + 1);
            // Path strip.
            if (auto dot = sub.find_last_of('.'); dot != std::string::npos) {
                sub = sub.substr(dot + 1);
            }
            // _C strip.
            if (sub.size() > 2 && sub.substr(sub.size() - 2) == "_C") {
                sub = sub.substr(0, sub.size() - 2);
            }
            // Emit with prefix matching the type mapper's heuristic.
            std::string fullName;
            if (sub.size() >= 2 &&
                (sub[0] == 'A' || sub[0] == 'U') &&
                sub[1] >= 'A' && sub[1] <= 'Z') {
                fullName = sub;
            } else if (sub == "Actor" ||
                       (sub.size() > 5 && sub.substr(sub.size() - 5) == "Actor") ||
                       sub == "Pawn" || sub == "Character" ||
                       sub == "Controller" || sub == "PlayerController" ||
                       sub == "PlayerState" || sub == "GameMode" ||
                       sub == "GameState" || sub == "HUD") {
                fullName = "A" + sub;
            } else {
                fullName = "U" + sub;
            }
            // Skip if it's the parent class (already #included transitively).
            if (fullName == parentClass) continue;
            forwardDecls.insert(fullName);
        }
    }
    for (const auto& fwd : forwardDecls) {
        H << "class " << fwd << ";\n";
    }
    if (!forwardDecls.empty()) H << "\n";

    // BP-implemented interfaces — emit them in the inheritance list.
    // UE convention: interface types are named `IFoo` (header form);
    // the BP-side reference uses the `UFoo` UClass shim. We accept
    // either form in BPIR and normalize to the `I` prefix.
    std::vector<std::string> interfaceClasses;
    if (doc.contains("interfaces") && doc["interfaces"].is_array()) {
        for (const auto& iface : doc["interfaces"]) {
            if (!iface.is_string()) continue;
            std::string name = iface.get<std::string>();
            // Strip /Script/Module. path prefix.
            if (auto dot = name.find_last_of('.'); dot != std::string::npos) {
                name = name.substr(dot + 1);
            }
            // Normalize prefix to `I`. BP can carry `BPI_X`, `UX`, `IX`,
            // or bare `X`.
            if (name.size() >= 2 && name[0] == 'I' &&
                name[1] >= 'A' && name[1] <= 'Z') {
                // Already I-prefixed.
            } else if (name.size() >= 2 && name[0] == 'U' &&
                       name[1] >= 'A' && name[1] <= 'Z') {
                name[0] = 'I';
            } else if (name.size() > 4 && name.substr(0, 4) == "BPI_") {
                name = "I" + name.substr(4);
            } else {
                name = "I" + name;
            }
            interfaceClasses.push_back(name);
        }
    }

    // Multicast-delegate property declarations. BP multicast delegate
    // variables come over the wire with type category `mcdelegate` /
    // `MulticastDelegate` -- not a real C++ type. UE's convention is
    // a DECLARE_DYNAMIC_MULTICAST_DELEGATE_NParams macro emitted
    // BEFORE the UCLASS body, with the resulting typedef used as the
    // property's type.
    //
    // We don't yet have signature info (param types) threaded from the
    // BP wire format to the BPIR class doc, so for now we emit
    // zero-param declarations + a TODO note pointing the user at the BP
    // signature graph. When BPIR carries a `delegates[]` field with the
    // signature (future PR), this loop will materialize the
    // _OneParam/_TwoParams/... variant with real types. The map below
    // is consulted by the variable-emission loop further down to
    // substitute the typedef name for the placeholder `mcdelegate`
    // type.
    auto isMcDelegateType = [](const std::string& s) {
        static const std::vector<std::string> tags = {
            "mcdelegate", "MulticastDelegate", "MulticastInlineDelegate"
        };
        for (const auto& tag : tags) {
            if (s == tag) return true;
            if (s.size() > tag.size() &&
                s.compare(0, tag.size(), tag) == 0 &&
                (s[tag.size()] == ':' || s[tag.size()] == '(')) {
                return true;
            }
        }
        return false;
    };
    auto deriveTypedefName = [&opts](const std::string& varName) {
        // Apply the user-configurable pattern. `{Name}` is the placeholder
        // for the variable's name. If the var already starts with F+upper
        // (Hungarian-prefixed by the BP author), strip the leading F
        // before substitution to avoid `FF<Name>` in default-pattern
        // mode.
        std::string token = varName;
        if (token.size() >= 2 && token[0] == 'F' &&
            token[1] >= 'A' && token[1] <= 'Z') {
            token = token.substr(1);
        }
        const std::string& pattern = opts.delegateTypedefPattern.empty()
            ? std::string("F{Name}")
            : opts.delegateTypedefPattern;
        std::string out;
        const std::string ph = "{Name}";
        std::size_t i = 0;
        while (i < pattern.size()) {
            if (pattern.compare(i, ph.size(), ph) == 0) {
                out += token;
                i += ph.size();
            } else {
                out += pattern[i++];
            }
        }
        return out;
    };
    std::map<std::string, std::string> mcDelegateTypedefs;  // varName -> typedefName
    if (doc.contains("variables") && doc["variables"].is_array()) {
        for (const auto& v : doc["variables"]) {
            std::string t = v.value("type", "");
            if (!isMcDelegateType(t)) continue;
            std::string varName = v.value("name", "Delegate");
            mcDelegateTypedefs[varName] = deriveTypedefName(varName);
        }
    }
    for (const auto& [varName, typedefName] : mcDelegateTypedefs) {
        H << "// TODO[bpr-delegate-signature]: fill in delegate params from BP "
             "signature graph (the zero-arg form below is a placeholder).\n";
        H << "DECLARE_DYNAMIC_MULTICAST_DELEGATE(" << typedefName << ");\n";
        nlohmann::json note;
        note["node_class"]    = "MulticastDelegateProperty";
        note["treatment"]     = "delegate_typedef_stub";
        note["delegate_name"] = typedefName;
        note["bp_var_name"]   = varName;
        note["hint"]          = "Replace DECLARE_DYNAMIC_MULTICAST_DELEGATE with "
                                "the _NParams variant matching the BP delegate "
                                "signature graph's parameter list.";
        out.notes.push_back(std::move(note));
    }
    if (!mcDelegateTypedefs.empty()) H << "\n";

    // .generated.h must be the LAST include in any UCLASS-bearing header.
    H << "#include \"" << cleanFileBase << ".generated.h\"\n\n";

    // UCLASS line. When the caller didn't pass a MODULE_API macro, fall
    // back to `UCLASS(MinimalAPI)` so other modules can still Cast<>
    // this class without us exporting every symbol — UE's standard
    // pattern for "Blueprintable but not part of the module's ABI".
    // When a real API macro is provided, plain `UCLASS(Blueprintable)`
    // with the macro on the class line is correct.
    //
    // opts.uclassMeta extends the macro with meta=(K1="V1", K2="V2"...)
    // -- folded in as a single trailing argument. Projects that need
    // `PrioritizeCategories="Foo"` or similar pass it via this map.
    std::string uclassMetaArg;
    if (!opts.uclassMeta.empty()) {
        uclassMetaArg = ", meta=(";
        bool first = true;
        for (const auto& [k, v] : opts.uclassMeta) {
            if (!first) uclassMetaArg += ", ";
            first = false;
            uclassMetaArg += fmt::format("{}=\"{}\"", k, v);
        }
        uclassMetaArg += ")";
    }
    if (opts.moduleApiMacro.empty()) {
        H << "UCLASS(MinimalAPI, Blueprintable" << uclassMetaArg << ")\n";
    } else {
        H << "UCLASS(Blueprintable" << uclassMetaArg << ")\n";
    }
    H << "class ";
    if (!opts.moduleApiMacro.empty()) H << opts.moduleApiMacro << " ";
    H << className << " : public " << parentClass;
    for (const auto& iface : interfaceClasses) {
        H << ", public " << iface;
    }
    H << " {\n";
    H << "    GENERATED_BODY()\n";
    H << "public:\n";

    // UPROPERTY decls.
    bool anyReplicated = false;
    if (doc.contains("variables") && doc["variables"].is_array()) {
        for (const auto& v : doc["variables"]) {
            // Build a patched view of the var so we can inject:
            //  - mcdelegate typedef substitution (`type` field)
            //  - category remap / default (`category` field)
            // RenderUPropertyDecl reads both fields directly so this
            // is the cleanest seam without changing the function's
            // public signature.
            nlohmann::json patched = v;
            auto td = mcDelegateTypedefs.find(v.value("name", ""));
            if (td != mcDelegateTypedefs.end()) {
                patched["type"] = td->second;
            }
            // Category: remap > default > original. Apply remap first
            // (project-specific normalization), then categoryDefault
            // when still empty. Skip both when no opts set, so the
            // legacy behavior is unchanged.
            std::string origCat = v.value("category", std::string{});
            std::string finalCat = origCat;
            if (!opts.categoryRemap.empty()) {
                if (auto it = opts.categoryRemap.find(origCat); it != opts.categoryRemap.end()) {
                    finalCat = it->second;
                }
            }
            if (finalCat.empty() && !opts.categoryDefault.empty()) {
                finalCat = opts.categoryDefault;
            }
            if (finalCat != origCat) {
                patched["category"] = finalCat;
            }
            H << RenderUPropertyDecl(patched);
            if (v.value("replicated", false)) anyReplicated = true;
        }
        if (!doc["variables"].empty()) H << "\n";
    }

    // Detect whether we're an Actor-derived class — drives both the
    // constructor (bReplicates) emit and the collision check against
    // reserved virtual methods.
    bool isActorDerived = !parentClass.empty() && parentClass[0] == 'A';

    // SCS-tracked components. These come from the BP's Construction
    // Script panel (and the SCS hierarchy) rather than from explicit
    // BP variables. The C++ binding pattern is:
    //   UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
    //   UStaticMeshComponent* MeshComponent;
    // ...then the constructor body creates them via
    // CreateDefaultSubobject<T>(TEXT("Name")) and wires parent attach.
    // Without this scaffold, the transpiled actor class has no
    // components even though the source BP does.
    bool hasComponents = doc.contains("components") &&
                        doc["components"].is_array() &&
                        !doc["components"].empty();
    if (hasComponents) {
        for (const auto& c : doc["components"]) {
            std::string nm  = c.value("name",  "Component");
            std::string cls = resolveComponentClass(c.value("class", ""));
            if (cls.empty()) cls = "UActorComponent";
            H << "    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category=\"Components\")\n";
            H << "    TObjectPtr<" << cls << "> " << nm << ";\n";
        }
        H << "\n";
    }

    // Constructor: required when the class needs runtime configuration
    // we can express in C++. Three reasons to emit one:
    //   - replicated vars (need `bReplicates = true`)
    //   - components (need CreateDefaultSubobject + attach setup)
    //   - both
    // Without one, replicated UPROPERTYs are silently broken and
    // components are silently missing.
    bool emitConstructor = (isActorDerived && anyReplicated) || hasComponents;
    if (emitConstructor) {
        H << "    " << className << "();\n\n";
    }

    // GetLifetimeReplicatedProps signature when any var is replicated.
    if (anyReplicated) {
        H << "    virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;\n\n";
    }

    // OnRep_X callbacks for any RepNotify variables. UE's reflection
    // requires these be UFUNCTION-decorated so the replication system
    // can find them by name and the BP can override them.
    if (doc.contains("variables") && doc["variables"].is_array()) {
        bool emittedAny = false;
        for (const auto& v : doc["variables"]) {
            std::string notifyFn;
            if (auto it = v.find("rep_notify_func");
                it != v.end() && it->is_string()) {
                notifyFn = it->get<std::string>();
            }
            if (notifyFn.empty()) continue;
            H << "    UFUNCTION()\n";
            H << "    void " << notifyFn << "();\n";
            emittedAny = true;
        }
        if (emittedAny) H << "\n";
    }

    // UFUNCTION decls + collision-warning sweep.
    if (doc.contains("functions") && doc["functions"].is_array()) {
        for (const auto& fn : doc["functions"]) {
            H << RenderUFunctionDecl(fn);
            // Collision check — emitting our own `UFUNCTION() bool
            // TakeDamage(float)` shadows the inherited UE virtual rather
            // than overriding it. Surface a sidecar note so the agent
            // knows to rename or convert to a real override.
            std::string fnName = fn.value("name", "");
            if (UeReservedMethodNames().count(fnName)) {
                nlohmann::json warn = {
                    {"node_class", "<name-collision>"},
                    {"function",   fnName},
                    {"parent",     parentClass},
                    {"reason",
                        fmt::format(
                            "Generated UFUNCTION '{}' shadows the inherited UE "
                            "virtual method on '{}'. Rename (e.g. '{}{}') or "
                            "refactor to the real override signature.",
                            fnName, parentClass, "Bp_", fnName)},
                    {"treatment", "todo_comment"},
                };
                out.notes.push_back(std::move(warn));
            }
        }
    }
    H << "};\n";
    out.headerSource = H.str();

    // ----- Impl generation --------------------------------------------
    std::ostringstream I;
    I << "// AUTO-GENERATED by bp-reader transpile. Do not edit — re-run to regenerate.\n\n";
    I << "#include \"" << out.headerFileName << "\"\n";
    if (anyReplicated) {
        I << "#include \"Net/UnrealNetwork.h\"\n";
    }
    I << "\n";

    // Constructor body. Three responsibilities (each optional):
    //   1. CreateDefaultSubobject + attach for each SCS component.
    //   2. RootComponent assignment when one component is flagged
    //      is_root.
    //   3. bReplicates = true when any UPROPERTY is replicated.
    // Without (1), the C++ actor is missing components the BP has.
    // Without (3), Replicated UPROPERTYs silently fail to replicate.
    if (emitConstructor) {
        I << className << "::" << className << "() {\n";

        if (hasComponents) {
            // First pass: instantiate every component. We do this in
            // declaration order so the root (if any) is created before
            // children that attach to it.
            std::string rootCompName;
            for (const auto& c : doc["components"]) {
                std::string nm  = c.value("name", "Component");
                std::string cls = resolveComponentClass(c.value("class", ""));
                if (cls.empty()) cls = "UActorComponent";
                I << "    " << nm << " = CreateDefaultSubobject<" << cls
                  << ">(TEXT(\"" << nm << "\"));\n";
                if (c.value("is_root", false)) rootCompName = nm;
            }
            // Second pass: attach non-root components to either their
            // explicit parent (if BP set one) or to the root.
            for (const auto& c : doc["components"]) {
                if (c.value("is_root", false)) continue;
                std::string nm = c.value("name", "Component");
                std::string parent = c.value("parent", std::string{});
                if (parent.empty() && !rootCompName.empty()) {
                    parent = rootCompName;
                }
                if (!parent.empty()) {
                    I << "    " << nm << "->SetupAttachment(" << parent << ");\n";
                }
            }
            // Wire the root explicitly. RootComponent assignment is
            // what makes UE treat the component as the actor's root.
            if (!rootCompName.empty()) {
                I << "    RootComponent = " << rootCompName << ";\n";
            }
        }

        if (anyReplicated) {
            I << "    bReplicates = true;\n";
        }
        I << "}\n\n";
    }

    if (anyReplicated) {
        I << "void " << className << "::GetLifetimeReplicatedProps("
          << "TArray<FLifetimeProperty>& OutLifetimeProps) const {\n";
        I << "    Super::GetLifetimeReplicatedProps(OutLifetimeProps);\n";
        if (doc.contains("variables") && doc["variables"].is_array()) {
            for (const auto& v : doc["variables"]) {
                if (!v.value("replicated", false)) continue;
                std::string varName = v.value("name", "Var");
                // BP's RepCondition field maps directly to a COND_*
                // specifier in DOREPLIFETIME_CONDITION. When the field
                // is absent or "None", emit unconditional DOREPLIFETIME.
                std::string repCond = v.value("rep_condition", std::string{});
                if (repCond.empty() || repCond == "None") {
                    I << "    DOREPLIFETIME(" << className << ", " << varName << ");\n";
                } else {
                    // Normalize to COND_ prefix. BP spells it
                    // "OwnerOnly" / "SkipOwner" / etc.; we accept either.
                    std::string condSpecifier =
                        (repCond.rfind("COND_", 0) == 0)
                            ? repCond
                            : ("COND_" + repCond);
                    I << "    DOREPLIFETIME_CONDITION(" << className
                      << ", " << varName << ", " << condSpecifier << ");\n";
                }
            }
        }
        I << "}\n\n";
    }

    if (doc.contains("functions") && doc["functions"].is_array()) {
        for (const auto& fn : doc["functions"]) {
            I << RenderUFunctionImpl(className, fn, opts.emitOpts, out.notes);
            I << "\n";
        }
    }

    out.implSource = I.str();
    return out;
}

} // namespace bpr::tools
