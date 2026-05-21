#include "tools/Prompts.h"

#include <fmt/core.h>

#include <algorithm>
#include <stdexcept>

namespace bpr::tools::prompts {

namespace prompts_detail {

nlohmann::json ArgumentSpec(const PromptArgument& a) {
	return nlohmann::json{
		{"name",        a.name},
		{"description", a.description},
		{"required",    a.required},
	};
}

}    // namespace prompts_detail
using namespace prompts_detail;

void PromptRegistry::Register(PromptDescriptor desc, PromptFn fn) {
	const std::string name = desc.name;
	// Replace-in-place semantics: re-register overwrites both the
	// descriptor and the handler. Mirrors ToolRegistry::Add.
	auto it = std::find_if(descriptors_.begin(), descriptors_.end(),
		[&](const PromptDescriptor& d) { return d.name == name; });
	if (it != descriptors_.end()) {
		*it = std::move(desc);
	} else {
		descriptors_.push_back(std::move(desc));
	}
	fns_[name] = std::move(fn);
}

nlohmann::json PromptRegistry::ListSpec() const {
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& d : descriptors_) {
		nlohmann::json entry = {
			{"name",        d.name},
			{"description", d.description},
		};
		if (!d.arguments.empty()) {
			nlohmann::json argsArr = nlohmann::json::array();
			for (const auto& a : d.arguments) {
				argsArr.push_back(ArgumentSpec(a));
			}
			entry["arguments"] = std::move(argsArr);
		}
		arr.push_back(std::move(entry));
	}
	return arr;
}

nlohmann::json PromptRegistry::Render(const std::string& name,
									   const nlohmann::json& arguments) const {
	auto it = fns_.find(name);
	if (it == fns_.end()) {
		throw std::invalid_argument(fmt::format("unknown prompt: {}", name));
	}
	auto descIt = std::find_if(descriptors_.begin(), descriptors_.end(),
		[&](const PromptDescriptor& d) { return d.name == name; });
	nlohmann::json messages = it->second(arguments);
	nlohmann::json result = {
		{"messages", std::move(messages)},
	};
	if (descIt != descriptors_.end() && !descIt->description.empty()) {
		result["description"] = descIt->description;
	}
	return result;
}

bool PromptRegistry::Has(const std::string& name) const {
	return fns_.count(name) > 0;
}

std::size_t PromptRegistry::Size() const {
	return descriptors_.size();
}

// ----------------------------------------------------------------------------
// Convenience helpers used by both built-in handlers and tests.
// ----------------------------------------------------------------------------

nlohmann::json UserMessage(std::string text) {
	return nlohmann::json{
		{"role", "user"},
		{"content", {
			{"type", "text"},
			{"text", std::move(text)},
		}},
	};
}

std::string RequirePromptArg(const nlohmann::json& args,
							  std::string_view promptName,
							  std::string_view argName) {
	if (!args.is_object()) {
		throw std::invalid_argument(fmt::format(
			"prompt '{}' requires an arguments object (got non-object)",
			promptName));
	}
	auto it = args.find(std::string(argName));
	if (it == args.end() || !it->is_string()) {
		throw std::invalid_argument(fmt::format(
			"prompt '{}' missing required string argument '{}'",
			promptName, argName));
	}
	return it->get<std::string>();
}

std::string OptionalPromptArg(const nlohmann::json& args,
							   std::string_view argName,
							   std::string fallback) {
	if (!args.is_object()) {
		return fallback;
	}
	auto it = args.find(std::string(argName));
	if (it == args.end() || !it->is_string()) {
		return fallback;
	}
	return it->get<std::string>();
}

// ----------------------------------------------------------------------------
// Built-in prompts
// ----------------------------------------------------------------------------
//
// Each prompt is a free-form workflow text the LLM uses to drive the
// agent through a multi-step pattern that's well-suited to the
// bp-reader-mcp tool surface. Bodies are intentionally specific —
// they name tools, suggest argument shapes, and warn about common
// gotchas — so the agent doesn't have to re-derive the recipe.
//
// Style notes:
//   - Tools referenced by their MCP name (e.g. `read_blueprint`).
//   - Asset paths use the canonical package form (`/Game/AI/BP_Foo`).
//   - "you" addresses the LLM, not the user.
//   - Each prompt's text fits in one user message — the LLM can read
//     it once and proceed. Long-form workflows get bullet structure
//     so the LLM can navigate.

void RegisterBuiltinPrompts(PromptRegistry& registry) {
	// ---- /audit_bp --------------------------------------------------------
	{
		PromptDescriptor d;
		d.name = "audit_bp";
		d.description =
			"Comprehensive BP quality audit: unused variables, missing "
			"categories, deep nesting, magic numbers. Returns a punch list "
			"ranked by severity.";
		d.arguments = {
			{"asset_path", "BP package path (e.g. /Game/AI/BP_Enemy).", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "audit_bp", "asset_path");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Audit the Blueprint at `{}` for quality issues. Work in this order:\n"
					"\n"
					"1. `read_blueprint` on the asset, then `list_variables` and "
					"`list_functions` to get the catalog.\n"
					"2. For each member variable, check:\n"
					"   - Is it referenced by any node? Use `find_node "
					"kind=VariableGet` and `kind=VariableSet` against its name.\n"
					"   - Does it have a non-empty `category`? Uncategorised "
					"variables clutter the BP detail panel.\n"
					"   - Is it `editable` but never used externally? Likely "
					"dead config.\n"
					"3. For each function:\n"
					"   - `get_function` with `summary: true` first to get the "
					"node count cheaply.\n"
					"   - Functions with >40 nodes are candidates for refactor.\n"
					"   - `find_dangling_references` to surface broken "
					"VariableGet/VariableSet/CallFunction nodes after rename "
					"or delete operations.\n"
					"4. Run `find_node kind=Literal` (and check numeric "
					"defaults on input pins) — float/int literals embedded in "
					"the graph are magic numbers; suggest hoisting to a "
					"member variable with a meaningful name.\n"
					"5. Check `comment` on top-level nodes — long branches "
					"with no comments are a smell.\n"
					"\n"
					"Output a punch list grouped by severity (BLOCKER / WARN / "
					"NIT) with file-relative anchors (`/Game/AI/BP_Enemy:func "
					"Foo:node <guid>`). Don't propose fixes inline — the user "
					"will follow up with `/suggest_refactor`.",
					asset)),
			});
		});
	}

	// ---- /explain_function ------------------------------------------------
	{
		PromptDescriptor d;
		d.name = "explain_function";
		d.description =
			"Walk a Blueprint function graph and explain its logic in plain "
			"English. Covers entry → return path, branch coverage, "
			"side-effects, and external dependencies.";
		d.arguments = {
			{"asset_path",    "BP package path.", /*required=*/true},
			{"function_name", "Function to explain (use the BP-side name, "
							  "not the C++ UFUNCTION).", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "explain_function", "asset_path");
			std::string fname = RequirePromptArg(args, "explain_function", "function_name");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Explain the `{}` function on Blueprint `{}` in plain "
					"English. Workflow:\n"
					"\n"
					"1. `get_function asset_path={} function_name={}` (no "
					"`summary` so you get pins + connections). If the node "
					"count is overwhelming, drop to `summary: true` and walk "
					"the high-level structure first.\n"
					"2. Identify the entry node (kind=FunctionEntry), the "
					"exit nodes (kind=FunctionResult), and any branches "
					"(kind=Branch) along the way.\n"
					"3. For each `CallFunction` node, note the "
					"`meta.targetFunction` — these are the calls the user "
					"would also need to reason about.\n"
					"4. Trace the exec chain from entry to result, noting "
					"which branches lead where. Don't enumerate every node — "
					"summarise the logical flow.\n"
					"5. Call out side-effects (writes to member variables, "
					"event dispatches, external function calls) and async "
					"work (timers, latent actions).\n"
					"\n"
					"Output structure: \"## What it does\" (1–2 sentences), "
					"\"## Inputs / outputs\" (parameter list), \"## Flow\" "
					"(numbered steps mirroring the exec chain), \"## "
					"Side-effects\".",
					fname, asset, asset, fname)),
			});
		});
	}

	// ---- /suggest_refactor ------------------------------------------------
	{
		PromptDescriptor d;
		d.name = "suggest_refactor";
		d.description =
			"Propose refactoring opportunities for a Blueprint based on graph "
			"analysis. Pairs with /audit_bp: that diagnoses, this prescribes.";
		d.arguments = {
			{"asset_path", "BP package path.", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "suggest_refactor", "asset_path");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Propose refactoring opportunities for Blueprint `{}`. "
					"Focus on agent-actionable changes — the user should be "
					"able to read your output and decide \"yes apply this\" "
					"or \"skip\". Workflow:\n"
					"\n"
					"1. Run `/audit_bp asset_path={}` first if you don't "
					"already have the punch list.\n"
					"2. Group findings into refactor opportunities:\n"
					"   - **Extract function:** any branch/sequence in the "
					"event graph with >15 nodes and a clean entry/exit pair.\n"
					"   - **Rename for clarity:** variables / functions with "
					"abbreviations, camelCase inconsistency, or generic "
					"names like `Temp`, `MyVar`.\n"
					"   - **Hoist literal:** magic numbers used in >1 place "
					"that should become a member variable.\n"
					"   - **Inline trivial function:** function with <5 "
					"nodes called from only 1 place.\n"
					"   - **Promote to C++:** function with a hot loop, "
					"complex math, or BPIR-compatible patterns. Mention "
					"`/transpile_to_cpp` as the next step.\n"
					"3. For each proposal, give: the change, the "
					"`apply_ops` batch shape you'd run (high-level — don't "
					"author the JSON yet), and the estimated impact.\n"
					"\n"
					"Don't apply anything. End your output with the literal "
					"line: \"Reply with the proposal numbers you want me to "
					"apply, or `apply all`.\"",
					asset, asset)),
			});
		});
	}

	// ---- /compare_blueprints ----------------------------------------------
	{
		PromptDescriptor d;
		d.name = "compare_blueprints";
		d.description =
			"Structural diff of two Blueprints with a plain-English "
			"explanation of the differences. Uses bp_structural_diff under "
			"the hood + decompile_blueprint for context.";
		d.arguments = {
			{"asset_a", "First BP package path.",  /*required=*/true},
			{"asset_b", "Second BP package path.", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string a = RequirePromptArg(args, "compare_blueprints", "asset_a");
			std::string b = RequirePromptArg(args, "compare_blueprints", "asset_b");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Compare Blueprints `{}` and `{}` and explain the "
					"differences. Workflow:\n"
					"\n"
					"1. `bp_structural_diff a={} b={}` — gives a node/pin/"
					"connection diff. Sort hits by graph then by node id.\n"
					"2. For high-noise diffs (>50 changes), follow up with "
					"`read_blueprint` on both and compare the variable + "
					"function lists directly — sometimes the diff is "
					"clearer at the API level than the graph level.\n"
					"3. Categorise differences:\n"
					"   - **Structural:** different parent class, different "
					"interface set, different member variables.\n"
					"   - **Graph-level:** added/removed/moved nodes in "
					"specific functions.\n"
					"   - **Wiring:** same nodes, different connections "
					"(often a bug indicator).\n"
					"   - **Cosmetic:** position changes only — call out "
					"and skip.\n"
					"4. For each meaningful difference, name the affected "
					"asset:graph:node and one-line the impact.\n"
					"\n"
					"Output structure: \"## Summary\" (one-paragraph "
					"high-level diff), \"## Structural\", \"## Graph-level\", "
					"\"## Wiring\". Omit empty sections.",
					a, b, a, b)),
			});
		});
	}

	// ---- /transpile_to_cpp ------------------------------------------------
	// THE MOAT prompt: walks the agent through decompile → review →
	// transpile → write workflow. This is what no other MCP server can
	// do, so the prompt body is intentionally specific about the
	// safeguards (BP_READER_ALLOW_TRANSPILE env, review-before-write,
	// BPIR diff).
	{
		PromptDescriptor d;
		d.name = "transpile_to_cpp";
		d.description =
			"Guide an agent through converting a Blueprint to C++ using the "
			"BPIR pivot: decompile → review → transpile → write_generated_source. "
			"Safe-by-default (writes gated on user confirmation).";
		d.arguments = {
			{"asset_path", "BP package path to transpile.", /*required=*/true},
			{"dest_path",  "Where the generated .cpp should land "
						   "(e.g. Source/MyGame/Public/MyBP.h). Optional — "
						   "defaults to Source/<ProjectName>/Generated/<BPName>.h.",
						   /*required=*/false},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "transpile_to_cpp", "asset_path");
			std::string dest  = OptionalPromptArg(args, "dest_path",
												  "Source/<project>/Generated/<bp>.h");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Transpile Blueprint `{}` to C++. The transpile family is "
					"OFF by default — first confirm `BP_READER_ALLOW_TRANSPILE=1` "
					"is set on the MCP server's env (call `get_project_metadata` "
					"and check the response — the tool will surface a clear "
					"error if the gate is closed). Then:\n"
					"\n"
					"1. **Decompile (read-only).** `decompile_blueprint "
					"asset_path={}` → BPIR JSON AST. Read this carefully. The "
					"BPIR's `unsupported` array names patterns the transpiler "
					"can't lower (custom events, latent actions, K2 macros, "
					"async tasks, etc.). If non-empty, **stop and tell the "
					"user** — incremental decomposition is needed.\n"
					"2. **Transpile (pure compute).** `transpile_blueprint "
					"asset_path={}` → generated C++ source as a string. No "
					"disk write yet.\n"
					"3. **Review.** Read the generated C++ inline. Watch for:\n"
					"   - `UFUNCTION` markers on every BP-callable function.\n"
					"   - `UPROPERTY` on every member that BP code might "
					"reference (even after transpile).\n"
					"   - `#include` lines pointing at the right modules — "
					"if the BP referenced a plugin class, the header path "
					"must reflect that.\n"
					"   - Naming: `B_` prefix for booleans, camelCase for "
					"functions, PascalCase for classes (UE conventions).\n"
					"4. **Confirm with the user** before writing. Show them "
					"a one-page summary: source path, header path, class "
					"name, deps. Get explicit yes.\n"
					"5. **Write.** `write_generated_source dest_path={} "
					"content=<the cpp text> create_dirs=true`. Then walk "
					"the user through Live Coding compilation in the editor.\n"
					"\n"
					"Don't transpile silently — surface the BPIR's unsupported "
					"set, show the generated code, and require confirmation. "
					"Transpile is destructive in the sense that it commits "
					"the user to a maintenance shift (BP edit → C++ edit). "
					"They should know what they're signing up for.",
					asset, asset, asset, dest)),
			});
		});
	}

	// ---- /review_generated_cpp --------------------------------------------
	{
		PromptDescriptor d;
		d.name = "review_generated_cpp";
		d.description =
			"After /transpile_to_cpp wrote generated C++, audit it for naming, "
			"UPROPERTY coverage, include hygiene, and BP↔C++ semantic drift.";
		d.arguments = {
			{"asset_path", "Source BP (used to cross-check semantic equivalence).",
						   /*required=*/true},
			{"cpp_path",   "Generated .cpp/.h to review.", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "review_generated_cpp", "asset_path");
			std::string cpp   = RequirePromptArg(args, "review_generated_cpp", "cpp_path");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Audit the generated C++ at `{}` against the source "
					"Blueprint `{}`. Workflow:\n"
					"\n"
					"1. Read the C++ file. Cross-reference function names + "
					"signatures against `list_functions asset_path={}`.\n"
					"2. **UPROPERTY coverage:** every member variable the BP "
					"declared must have a matching `UPROPERTY(...)` in C++. "
					"Run `list_variables` on the BP and check each name is "
					"present and the type matches.\n"
					"3. **UFUNCTION markers:** every BP-callable function "
					"needs `UFUNCTION(BlueprintCallable, ...)`. Pure "
					"functions get `BlueprintPure`. Events get "
					"`BlueprintImplementableEvent` or `BlueprintNativeEvent`.\n"
					"4. **Include hygiene:** verify `#include` lines point "
					"at real headers. Watch for `#include \"Engine/Texture.h\"` "
					"missing when the BP references textures (we hit this "
					"bug in 0083dffc). Use `find_class` to verify each "
					"referenced UClass is reachable.\n"
					"5. **Naming + style:** PascalCase classes, camelCase "
					"functions, `b` prefix on bools (UE convention). Flag "
					"deviations.\n"
					"6. **Semantic check:** use `parse_cpp_function` to "
					"round-trip a critical function back to BPIR, then `bp_"
					"structural_diff` BPIR-A vs BPIR-B. Zero structural diff "
					"= semantically equivalent.\n"
					"\n"
					"Output a punch list (BLOCKER / WARN / NIT). End with a "
					"verdict: \"safe to compile\", \"fix BLOCKERs first\", or "
					"\"re-transpile — too much drift\".",
					cpp, asset, asset)),
			});
		});
	}

	// ---- /check_transpile_compat ------------------------------------------
	{
		PromptDescriptor d;
		d.name = "check_transpile_compat";
		d.description =
			"Pre-flight check for /transpile_to_cpp: identifies BP patterns "
			"the transpiler can't lower cleanly. Run this BEFORE transpiling.";
		d.arguments = {
			{"asset_path", "BP package path.", /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "check_transpile_compat", "asset_path");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Pre-flight check whether Blueprint `{}` will transpile "
					"cleanly to C++. Don't actually transpile — just look at "
					"what's there and predict the issues. Workflow:\n"
					"\n"
					"1. `read_blueprint asset_path={}` for the top-level "
					"shape — parent class, interfaces, variables.\n"
					"2. For each function, `get_function summary=true` to "
					"see the node kinds in play.\n"
					"3. Flag these patterns as transpile-hard:\n"
					"   - **Latent actions:** Delay, MoveComponentTo, "
					"PlayAnimation in EventTick — async UFUNCTIONs need "
					"manual rewrite.\n"
					"   - **K2 macros:** ForEachLoop, ForEachLoopWithBreak, "
					"Gate — these expand at compile time and the transpiler "
					"reproduces the expansion, which gets verbose.\n"
					"   - **Timeline nodes:** Timeline assets need separate "
					"transpile or kept as UPROPERTY references.\n"
					"   - **Custom events with delegates bound:** the "
					"binding side stays BP — partial transpile only.\n"
					"   - **Cross-BP function calls:** the called BP must "
					"also exist as C++ (or stay loaded at runtime).\n"
					"   - **Async tasks (UK2Node_BaseAsyncTask):** require "
					"manual UCLASS subclass to host the static factory + "
					"delegate signature.\n"
					"4. Compute a transpile-safety score:\n"
					"   - **GREEN:** pure compute + variable get/set + "
					"straight CallFunction chains → safe to transpile.\n"
					"   - **YELLOW:** has 1-2 of the above patterns → "
					"transpile with manual cleanup expected.\n"
					"   - **RED:** has 3+ patterns OR latent actions in hot "
					"paths → don't transpile this BP whole; extract pure "
					"sub-functions and transpile those.\n"
					"\n"
					"Output the score, then the specific BPIR patterns you "
					"saw with anchors. Recommend `/transpile_to_cpp` for "
					"GREEN, partial transpile for YELLOW, and full BP-side "
					"refactor first for RED.",
					asset, asset)),
			});
		});
	}

	// ---- /lyra_gameplay_review --------------------------------------------
	{
		PromptDescriptor d;
		d.name = "lyra_gameplay_review";
		d.description =
			"Lyra-specific BP review: checks GAS integration, GameFeaturePlugin "
			"boundaries, ModularGameplay conventions, and Lyra's preferred "
			"composition patterns.";
		d.arguments = {
			{"asset_path", "BP package path to review against Lyra conventions.",
						   /*required=*/true},
		};
		registry.Register(std::move(d), [](const nlohmann::json& args) {
			std::string asset = RequirePromptArg(args, "lyra_gameplay_review", "asset_path");
			return nlohmann::json::array({
				UserMessage(fmt::format(
					"Review Blueprint `{}` against Lyra-style gameplay "
					"conventions. The host project is the Lyra Starter Game, "
					"so the BP should fit those patterns. Workflow:\n"
					"\n"
					"1. `read_blueprint asset_path={}` for the top-level "
					"shape. Note parent class + interfaces.\n"
					"2. **Parent class check:** Lyra prefers composition "
					"over inheritance. Flag deep inheritance chains (>3 "
					"levels above ALyraCharacter / AModularCharacter / "
					"etc.) — usually a sign that a `Components` array "
					"would be cleaner.\n"
					"3. **GAS integration:** if the BP has ability "
					"references, check it follows Lyra's pattern:\n"
					"   - Abilities granted via `ULyraAbilitySet`, not "
					"manually in BeginPlay.\n"
					"   - GameplayEffects applied via tag handles, not "
					"hard-coded references.\n"
					"   - Tags fetched via `Gameplay.Native.Tags`, not "
					"string literals.\n"
					"4. **GameFeaturePlugin (GFP) boundaries:** if the BP "
					"lives under `/Game/<Feature>/...`, check that:\n"
					"   - It references only assets under the same feature "
					"OR `/Game/System/` (shared layer).\n"
					"   - Cross-feature references go through Subsystem "
					"interfaces, not direct asset paths.\n"
					"5. **ModularGameplay conventions:**\n"
					"   - Actors compose `ULyraEquipmentInstance`, "
					"`ULyraInventoryItemInstance`, etc., instead of "
					"hard-coded fields.\n"
					"   - Input via `ULyraInputComponent` + IMC, not "
					"hard-coded `EnhancedInputAction` references.\n"
					"6. **Lyra-specific anti-patterns:** Tick logic that "
					"could be GAS attribute changes; magic IDs that should "
					"be GameplayTags; setup logic in BeginPlay that should "
					"be on a Component spawn callback.\n"
					"\n"
					"Output structure: \"## Conventions matched\" (what's "
					"good), \"## Convention misses\" (BLOCKER/WARN/NIT punch "
					"list), \"## Recommendations\" (concrete refactor or "
					"`/suggest_refactor` follow-up).",
					asset, asset)),
			});
		});
	}
}

}    // namespace bpr::tools::prompts
