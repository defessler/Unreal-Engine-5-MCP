#!/usr/bin/env bash
# One-off helper: file every bug fixed in the May 2026 transpiler-upgrade
# session as a GitHub issue, then close each one as `completed` with the
# commit reference. Idempotent-ish: re-running would create duplicates,
# so it's intended to be run exactly once after the session.
#
# Usage: bash file-session-bug-issues.sh
#
# Requires: gh (authenticated), run from any path inside the repo.

set -eu

cd "$(dirname "$0")/../../.."

file_issue() {
    local title="$1"
    local body="$2"
    local commit="$3"
    local pr="$4"
    local full_body
    full_body="${body}

**Fixed in:** commit \`${commit}\` (${pr})

This issue was filed retroactively as part of a session-end audit pass
to create a paper trail for every bug fixed during the May 2026
transpiler upgrade session."
    local n
    n=$(gh issue create --title "${title}" --body "${full_body}" | tail -1 | grep -oE '[0-9]+$')
    gh issue close "${n}" --reason completed --comment "Closed by commit ${commit} (${pr})." > /dev/null
    echo "Filed + closed #${n}: ${title}"
}

# ----- Diagnostic findings (#1 - #15) -----------------------------------
file_issue \
    "bug(transpile): qualified BP paths leak into C++ identifiers" \
    "BPIR stores types and function refs as fully-qualified asset paths (\`/Script/Mod.Class\`, \`/Game/Path.Asset_C\`). CppEmit printed those verbatim where it expected a C++ identifier. Example: \`/Script/Engine.KismetSystemLibrary::PrintString(...)\` instead of \`KismetSystemLibrary::PrintString(...)\`." \
    "e95d13c" "PR #104"

file_issue \
    "bug(transpile): mcdelegate placeholder type emitted for multicast delegate properties" \
    "BP multicast-delegate variables arrived over the wire with type category \`mcdelegate\` / \`MulticastDelegate\` -- not a real C++ type. CppClassEmit passed the placeholder through verbatim, producing \`mcdelegate OnSomethingHappened;\` which doesn't compile. Fix emits \`DECLARE_DYNAMIC_MULTICAST_DELEGATE(F<Name>)\` + uses the F-prefixed typedef as the member type." \
    "fcc6c96" "PR #105"

file_issue \
    "bug(transpile): K2Node_CallDelegate / Add / Remove / Clear have no treatment" \
    "All four delegate-op K2Node classes fell through to the TODO sentinel path. Without lowering, BP authors using multicast delegates got \`// TODO[bpr-unsupported]: K2Node_CallDelegate\` comments where the actual \`Broadcast() / AddDynamic() / RemoveDynamic() / Clear()\` calls should be." \
    "26d3567" "PR #103"

file_issue \
    "bug(transpile): IsValid macro instance has no treatment" \
    "Five out of seven K2Node_MacroInstance occurrences in the diagnostic asset were the standard \`IsValid\` macro. CppEmit emitted \`// TODO[bpr-unsupported]\` for each one, leaving control-flow gaps everywhere \`IsValid\` appeared." \
    "26d3567" "PR #103"

file_issue \
    "bug(transpile): multi-output BP function emits std::make_tuple (not in UE allowed stdlib)" \
    "Decompile + CppEmit lowered multi-output BP functions to \`return std::make_tuple(...);\`. UE's allowed stdlib subset doesn't include \`std::make_tuple\`; the canonical UE pattern is reference-out parameters." \
    "26d3567" "PR #103"

file_issue \
    "bug(transpile): FunctionEntry pin not resolved to parameter name" \
    "When a K2Node_VariableSet consumed a pin driven by a K2Node_FunctionEntry output, decompile reported the upstream node's class as the expression source instead of resolving back to the entry pin's named output (the parameter name). CppEmit then printed a TODO sentinel because BPIR had no var: form pointing at the entry pin." \
    "e95d13c" "PR #104"

file_issue \
    "bug(transpile): cross-BP function calls render as raw asset paths" \
    "Calls to BP-only functions on other widget BPs (e.g. \`WB_ResourceList::SetResources\`) rendered as the full asset path qualifier (\`/Game/UI/X.WB_X_C::Set Resources\`). No C++ symbol exists for these. Fix routes BP-only targets through a UFunction reflection lookup + ProcessEvent dispatch." \
    "e95d13c" "PR #104"

file_issue \
    "bug(transpile): BP function display name with spaces emitted verbatim" \
    "A BP function whose display name was \`Set Resources\` (with a space) emitted the literal \`Set Resources(...)\` in C++ -- not a legal identifier. SanitizeIdentifier now collapses non-\`[A-Za-z0-9_]\` chars at every identifier emission site." \
    "26d3567" "PR #103 + PR #104"

file_issue \
    "bug(transpile): Cast<T> target left as full asset path" \
    "\`Cast</Game/UI/Widgets/.../WB_ResourceBar_C>(...)\` would leak into the output. ResolveAssetPath now strips \`/Script/Module.\` and \`/Game/Path.\` prefixes (and the trailing \`_C\`) at every Cast<T> emission point." \
    "e95d13c" "PR #104"

file_issue \
    "bug(transpile): Cast-to local variable name contains spaces" \
    "BP cast nodes generate a \`AsX\` local whose default name is derived from the target class's display name -- BP authors can leave spaces in. Output was \`auto* AsExample Bar = Cast<...>(...)\`, illegal identifier." \
    "26d3567" "PR #103"

file_issue \
    "bug(transpile): Sequence node rendered as // sequence branch N comment markers" \
    "K2Node_ExecutionSequence emission added \`// sequence branch 0\`, \`// sequence branch 1\` etc. comments before each branch's statements. Sequence is structurally equivalent to ordered statements -- the markers were pure noise. Empty trailing branches also produced empty marker comments." \
    "26d3567" "PR #103"

file_issue \
    "bug(transpile): generated class violates project naming convention (no class_name_prefix arg)" \
    "transpile_blueprint had no knob to inject a project's house naming prefix between UE's type letter (A/U/I) and the BP's CamelCased name. \`BP_Enemy\` -> \`ABP_Enemy_Generated\` was the only output shape; projects with house conventions had to post-process." \
    "806a111" "PR #106"

file_issue \
    "bug(transpile): UPROPERTY categories preserved verbatim (no remap)" \
    "Every UPROPERTY kept the BP-authored Category string. Projects with house categorization (e.g. \`Polaris\`, \`MyGame|Subsection\`) had to manually rewrite each \`Category=\"Default\"\` / \`Category=\"Internal State\"\`. Added \`category_default\` + \`category_remap\` args." \
    "806a111" "PR #106"

file_issue \
    "bug(transpile): UCLASS missing project-required meta (no uclass_meta arg)" \
    "Some projects require \`UCLASS(..., meta=(PrioritizeCategories=\"...\"))\` on every generated class. transpile_blueprint had no way to fold those entries into the UCLASS macro. Added \`uclass_meta\` arg." \
    "806a111" "PR #106"

file_issue \
    "bug(transpile): parameter shadows member, no this-> disambiguation in setter bodies" \
    "BP \`SetPlayer(Player)\` storing the parameter into a matching member emitted \`Player = Player;\` -- a self-assignment, not the intended member write. CppEmit now tracks param + local + output names and prefixes \`this->\` on member-scope LHS that collides with a parameter." \
    "e95d13c" "PR #104"

# ----- Session script / build fixes -------------------------------------
file_issue \
    "bug(scripts): Polaris-style downstream hit 'parameter cannot be found that matches parameter name ProjectDir'" \
    "PR #75 removed the PreBuildStep from BlueprintReader.uplugin, but UBT caches PreBuild invocations as Intermediate/Build/.../PreBuild-N.bat files. Those cached scripts still called Build-MCPServer.ps1 with the pre-UBT contract (-ProjectDir / -PluginDir) until UBT regenerated them. Build broke for users who pulled the plugin without regenerating project files." \
    "de46de2" "session fix (no PR)"

file_issue \
    "bug(scripts): plugin scripts referenced post-PR-#75 stale paths" \
    "Three plugin scripts (Build-MCPServer.bat, Start-MCPServer.ps1, Verify-Build.ps1) still referenced the pre-UBT CMake build paths + PreBuildStep model. Caused build/launch breakage for users on current plugin." \
    "68c8072" "session fix (no PR)"

file_issue \
    "bug(scripts): Start-MCPServer.ps1 'exe not found' message showed -project=\"True\" instead of real path" \
    "PowerShell's \`-or\` operator is boolean, not null-coalescing. The expression \`\$uproject -or '<placeholder>'\` evaluated to literal \`True\`, polluting the build hint in the diagnostic." \
    "80ac976" "PR #99"

file_issue \
    "bug(scripts): Verify-Build.ps1 'Fix' advice was stale post-PR-#97" \
    "The 'Fix for missing BlueprintReaderMcp.exe' section still said 'no longer a PreBuildStep of the editor build'. PR #97 restored the PreBuildStep via .uplugin -- the advice contradicted current reality." \
    "7ee0b91" "PR #101"

# ----- Build / PreBuild infrastructure ----------------------------------
file_issue \
    "bug(build): UBT mutex deadlock in nested PreBuildHook UBT invocation" \
    "PreBuildHook.ps1 invoking Build.bat for BlueprintReaderMcp from inside the editor's PreBuildSteps deadlocked: parent UBT held the global mutex while waiting on the PreBuildStep; child UBT blocked on the same mutex. Fix passes \`-NoMutex\` to the child invocation; intermediate dirs don't collide so concurrent UBT is safe." \
    "bd95a50" "PR #97"

file_issue \
    "bug(build): UBT log-file collision in nested PreBuildHook UBT invocation" \
    "Parent UBT holds Engine/Programs/UnrealBuildTool/Log.txt open exclusively. Child UBT (spawned by PreBuildHook) errored on its log-file backup-rename. Fix points the child at \`\$env:TEMP/UnrealBuildTool-BlueprintReaderMcp.log\` via \`-Log=\`." \
    "bd95a50" "PR #97"

file_issue \
    "bug(build): PreBuildSteps recursion when building plugin's own Program targets" \
    "The plugin's .uplugin PreBuildSteps fire for every target that uses the plugin -- including the BlueprintReaderMcp and BlueprintReaderMcpTests Program targets themselves. Without a guard the hook would loop forever. Fix: PreBuildHook.ps1 reads \$(TargetName) and exits 0 cleanly when the parent target is one of the plugin's own Program targets." \
    "bd95a50" "PR #97"

file_issue \
    "bug(scripts): em dashes in .ps1 files broke parsing under powershell.exe -File" \
    "Windows PowerShell 5.1 (invoked by UBT's PreBuild-N.bat via cmd) reads BOM-less .ps1 files in the system ANSI codepage. An em dash (UTF-8 e2 80 94) decoded as CP-1252 \`â€\"\` -- the embedded right-quote terminated the surrounding string mid-line, breaking the script's syntax. Replaced em dashes / smart quotes / NBSPs / ellipsis with ASCII equivalents across all six plugin scripts." \
    "bd95a50" "PR #97"

file_issue \
    "bug(verify): stale cached PreBuild-N.bat undetectable from Verify-Build output" \
    "When a user updated the plugin but UBT's cached PreBuild-N.bat in Intermediate/ still referenced the old hook path, the editor build silently skipped the MCP server build. Verify-Build.ps1 reported clean source files + missing exe but couldn't explain why. PR #101 adds a cache-diagnostic pass that reads each cached PreBuild-N.bat and reports whether PreBuildHook.ps1 is referenced." \
    "7ee0b91" "PR #101"

# ----- Dev-loop bugs (caught + fixed during development) ----------------
file_issue \
    "bug(transpile/dev): const_cast on doc to mutate component-defaults map" \
    "An initial cut at the mcdelegate handler used const_cast on the doc reference to mutate a variable's type field for the rendering loop. That's a code smell -- mutating a const-referenced doc. Refactored to build a separate name->typedef map and patch a local copy of each var doc before calling RenderUPropertyDecl." \
    "fcc6c96" "PR #105 (caught during dev)"

file_issue \
    "bug(transpile/dev): variable shadowing in __bpr_select_n emit (Emitter::opts, Emitter::out)" \
    "Initial __bpr_select_n implementation used local variables named \`opts\` and \`out\` inside an Emitter method. Those names shadowed Emitter member variables of the same name, triggering MSVC's C4458. Renamed to \`selectOpts\` / \`chain\`." \
    "5b8469f" "PR #112 (caught during dev)"

file_issue \
    "bug(transpile/dev): missing #include <algorithm> after introducing std::sort" \
    "Added \`std::sort\` to Decompile.cpp (Select node handler) and CppEmit.cpp (select-n emit) without including \`<algorithm>\`. Caught by the next build cycle; added the includes." \
    "5b8469f" "PR #112 (caught during dev)"

file_issue \
    "bug(transpile/dev): RPC tests omitted required version field on function sub-docs" \
    "Initial RPC \`_Implementation\` tests passed function objects without the required \`{version, kind}\` fields. CppClassEmit's EmitCppFunctionBody runs ValidateBpir which threw \`BPIR doc requires integer 'version' field\`. Added the missing fields to test fixtures." \
    "0b5c1ee" "PR #107 (caught during dev)"

file_issue \
    "bug(transpile/dev): SCS components test assertion too broad (matched GENERATED_BODY)" \
    "An initial test asserted \`CHECK_FALSE(Contains(headerSource, '_C'))\` to verify _C suffix stripping. The header legitimately contains \`GENERATED_BODY\` and other \`_C\`-containing tokens. Narrowed to \`CHECK_FALSE(Contains(headerSource, 'BPC_Custom_C'))\` matching the specific stripped name." \
    "78ce732" "PR #108 (caught during dev)"

file_issue \
    "bug(transpile/dev): test fixture count assertion stale after adding BP_ExampleCharacter" \
    "Adding the new comprehensive BP_ExampleCharacter fixture bumped the mock fixture count from 3 to 4. Three tests (test_mcp.cpp / test_mock_backend.cpp / test_tools.cpp) hardcoded \`== 3\` and started failing. Updated to \`== 4\`." \
    "0db1d41" "PR #115 (caught during dev)"

# ----- Today's bug-hunt fixes -------------------------------------------
file_issue \
    "bug(transpile): class-level emitters don't sanitize identifiers" \
    "Caught during Pass 2 of a 20-pass grep audit. RenderUPropertyDecl, RenderUFunctionDecl, RenderUFunctionImpl read names from BPIR and emitted them verbatim. A BP variable named \"Selected Plot\" leaked as \`float Selected Plot;\` -- doesn't compile. Stage 1's SanitizeIdentifier was in CppEmit.cpp's anon namespace; CppClassEmit (separate TU) couldn't reach it. Exposed through CppEmit.h." \
    "42a873d" "PR #116"

file_issue \
    "bug(transpile): TEXT(...) emissions don't escape embedded quote or backslash" \
    "Caught during Pass 18 of a 20-pass grep audit. A BP string default \`Hello \"world\"\` leaked as \`TEXT(\"Hello \"world\"\")\` -- doesn't compile. Backslashes leaked as \`TEXT(\"C:\\\\Path\\\\file\")\` -- interpreted as escape sequences. Added EscapeForCppStringLiteral helper; applied at the 4 emission sites in CppEmit.cpp + CppClassEmit.cpp." \
    "42a873d" "PR #116"

echo
echo "All issues filed + closed."
