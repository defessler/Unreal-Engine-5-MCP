#include "tools/codegen/CppClassEmit.h"
#include "tools/Bpir.h"

#include <fmt/core.h>

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

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

std::string BuildUPropertyList(const nlohmann::json& varDecl) {
    std::vector<std::string> specs;
    if (varDecl.value("editable", false)) {
        specs.push_back("EditAnywhere");
    }
    // BP variables are always Blueprint-visible.
    specs.push_back("BlueprintReadWrite");
    if (varDecl.value("replicated", false)) {
        specs.push_back("Replicated");
    }
    if (auto it = varDecl.find("category"); it != varDecl.end() && it->is_string()
        && !it->get_ref<const std::string&>().empty()) {
        specs.push_back(fmt::format(R"(Category="{}")", it->get<std::string>()));
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
    }
    if (!sawExplicit) {
        specs.push_back("BlueprintCallable");
    }
    std::string out;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (i) out += ", ";
        out += specs[i];
    }
    return out;
}

namespace {

// Render a BPIR variable as a UPROPERTY decl + initializer.
//   UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Stats", Replicated)
//   float Health = 100.0f;
std::string RenderUPropertyDecl(const nlohmann::json& v) {
    std::string specs = BuildUPropertyList(v);
    std::string typeStr = v.value("type", "void");
    std::string cppType = MapBpirTypeToCpp(typeStr);
    std::string name    = v.value("name", "Var");
    std::string defaultStr = v.value("default", std::string{});
    std::string line = fmt::format("    UPROPERTY({})\n    {} {}", specs, cppType, name);
    if (!defaultStr.empty()) {
        // Defensive: we can't parse arbitrary BP default strings to
        // C++ initializers, but for primitives the BP serialization
        // matches C++ literal syntax closely enough (e.g. "100.000000"
        // → "100.000000f", "true" → "true", "/Script/..." object refs
        // we punt on).
        if (defaultStr == "true" || defaultStr == "false") {
            line += fmt::format(" = {}", defaultStr);
        } else if (cppType == "float" || cppType == "double") {
            line += fmt::format(" = {}f", defaultStr);
        } else if (cppType.find("int") == 0 || cppType == "uint8") {
            // Strip trailing decimals from BP's "0.000000" form on int fields.
            std::string trimmed = defaultStr;
            auto dot = trimmed.find('.');
            if (dot != std::string::npos) trimmed = trimmed.substr(0, dot);
            line += fmt::format(" = {}", trimmed);
        }
        // Other types: skip the inline default. Constructor body or
        // BeginPlay can set them later.
    }
    line += ";\n";
    return line;
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
            args += MapBpirTypeToCpp(in.value("type", "void"));
            args += " ";
            args += in.value("name", "Arg");
        }
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() > 1) {
        for (const auto& out : fn["outputs"]) {
            if (!args.empty()) args += ", ";
            args += MapBpirTypeToCpp(out.value("type", "void"));
            args += "& ";
            args += out.value("name", "Out");
        }
    }
    std::string fnName = fn.value("name", "Fn");
    return fmt::format("    UFUNCTION({})\n    {} {}({});\n", specs, returnType, fnName, args);
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
            args += MapBpirTypeToCpp(in.value("type", "void"));
            args += " ";
            args += in.value("name", "Arg");
        }
    }
    if (fn.contains("outputs") && fn["outputs"].is_array() &&
        fn["outputs"].size() > 1) {
        for (const auto& out : fn["outputs"]) {
            if (!args.empty()) args += ", ";
            args += MapBpirTypeToCpp(out.value("type", "void"));
            args += "& ";
            args += out.value("name", "Out");
        }
    }
    std::string fnName = fn.value("name", "Fn");

    auto bodyResult = EmitCppFunctionBody(fn, emitOpts);
    for (const auto& n : bodyResult.notes) accumulatedNotes.push_back(n);

    return fmt::format(
        "{} {}::{}({}) {{\n{}}}\n",
        returnType, className, fnName, args, bodyResult.source);
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
    std::string baseClassName = PrefixClassName(bpName, parentClass);
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

    // .generated.h must be the LAST include in any UCLASS-bearing header.
    H << "#include \"" << cleanFileBase << ".generated.h\"\n\n";

    // UCLASS line.
    H << "UCLASS(Blueprintable)\n";
    H << "class ";
    if (!opts.moduleApiMacro.empty()) H << opts.moduleApiMacro << " ";
    H << className << " : public " << parentClass << " {\n";
    H << "    GENERATED_BODY()\n";
    H << "public:\n";

    // UPROPERTY decls.
    bool anyReplicated = false;
    if (doc.contains("variables") && doc["variables"].is_array()) {
        for (const auto& v : doc["variables"]) {
            H << RenderUPropertyDecl(v);
            if (v.value("replicated", false)) anyReplicated = true;
        }
        if (!doc["variables"].empty()) H << "\n";
    }

    // GetLifetimeReplicatedProps signature when any var is replicated.
    if (anyReplicated) {
        H << "    virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;\n\n";
    }

    // UFUNCTION decls.
    if (doc.contains("functions") && doc["functions"].is_array()) {
        for (const auto& fn : doc["functions"]) {
            H << RenderUFunctionDecl(fn);
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

    if (anyReplicated) {
        I << "void " << className << "::GetLifetimeReplicatedProps("
          << "TArray<FLifetimeProperty>& OutLifetimeProps) const {\n";
        I << "    Super::GetLifetimeReplicatedProps(OutLifetimeProps);\n";
        if (doc.contains("variables") && doc["variables"].is_array()) {
            for (const auto& v : doc["variables"]) {
                if (v.value("replicated", false)) {
                    I << "    DOREPLIFETIME(" << className << ", "
                      << v.value("name", "Var") << ");\n";
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
