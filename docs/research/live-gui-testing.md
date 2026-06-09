# Testing the full tool suite against a render-capable / live GUI editor

**Status:** research + plan (2026-06-08). Not yet implemented.

The `-nullrhi` commandlet daemon the test harness uses cannot exercise the
~30 render/interactive tools: they gate on `FApp::CanEverRender()`, which is
`false` under `-nullrhi`, so `take_screenshot`, `take_viewport_screenshot`,
`take_annotated_screenshot`, `set_camera_transform`, `set_show_flag`,
`build_lighting`, and real `pie_start`/`pie_stop` only get a *registration*
check (they honestly return `captured/started/moved:false`). This doc is how to
close that gap.

## Key finding: the "wedging modal" was a missing flag, not a wall

The Slate modal that wedged an earlier GUI-editor launch (a window titled
"Warning") is an `FMessageDialog` — the **"modules are missing or built with a
different engine version — rebuild now? [Yes][No]"** prompt. Per Epic's docs,
`FMessageDialog` **auto-returns its default and never blocks when the process
is `-unattended`**. The headless daemon already passes `-unattended`
(`Saved/start-daemon.ps1`) and never wedges — confirming the diagnosis. The
earlier GUI attempt simply didn't pass `-unattended`. So the GUI editor *is*
automatable; UIAutomation never needs to "click" the Slate dialog.

The second key finding: **`-AllowCommandletRendering`** (App.h) flips
`FApp::CanEverRender()` to `true` while keeping the headless, single-process,
Session-0-safe daemon the project already trusts — potentially un-skipping the
render subset (minus PIE) on the **existing** daemon with no GUI at all.

## Fidelity requirement (the bar this must clear)

**The test must reproduce how each tool responds when a human is actively
using the editor — not a headless approximation that merely doesn't error.**
This is the deciding criterion, and it splits the tools into two groups:

- **State-independent** (read/write BP graphs, vars, functions, assets, data
  tables, etc.): already faithful on the headless daemon — the BP/asset state
  is identical whether or not an artist is at the keyboard. No new work needed
  for fidelity here; the existing commandlet coverage is representative.
- **Active-editor-state-dependent** — these only respond *accurately* against a
  real, open, in-use editor with a **map loaded + an active level viewport +
  real selection/world state**, because that state is exactly what they read:
  `take_viewport_screenshot`, `set_camera_transform`, `focus_actor`,
  `set_show_flag` (active viewport); `get_selected_actors`/`set_selection`,
  `get_editor_state` (live selection/UI state); `open_asset_editor`;
  `pie_start`/`pie_stop` (needs the editor to host the play world);
  `spawn_actor`/`set_actor_transform`/`read_actor_instance` (a real loaded
  world). A render-capable *commandlet* (`-AllowCommandletRendering`) flips
  `CanEverRender()` true but **has no active viewport, no selection, no loaded
  map, and can't host PIE** — so for THIS group its responses do **not** match
  active use. They would "succeed" against a synthetic/no viewport, which is a
  false positive against the bar above.

**Implication:** for the state-dependent group, fidelity requires **Track B**
(a real GUI editor with a level open and a live viewport, driven via the `live`
backend — the exact configuration a human has when they use the MCP server
interactively). Track A is only faithful for render captures that don't depend
on the active viewport/selection (e.g. `build_lighting` against a loaded map,
or a HighResShot of an explicitly opened scene) — it is a partial measure, not
a substitute. The live smoke must also **put the editor in a representative
state first** (load a test map, select an actor, open an asset) so the tools
read the same world an active user's editor would present.

## Two tracks

### Track A — render-capable headless daemon (the CI win; do first)
Launch the same `-run=BPR -Daemon` daemon but **drop `-nullrhi`** and add
**`-AllowCommandletRendering`**, plus a real GPU *or* `-RenderOffScreen -dx12
-WARP` on a GPU-free box. No Slate, no autologon, survives Session 0 — fully
CI-automatable on the existing self-hosted `ue5` runner (`editor-build.yml`).
Unlocks (pending the Phase-0 spike): screenshots, `build_lighting`, camera,
show-flags — anything needing an RHI but not a full editor viewport/PIE.

- **Load-bearing unknown:** does the editor actually render under
  `-RenderOffScreen -dx12 -WARP` (software)? Epic documents WARP only for
  packaged `-Cmd` games, not the editor. A real (even modest) GPU removes this
  risk entirely. **A commandlet has no editor viewport**, so viewport-scoped
  captures may need a loaded map/scene — the spike settles which tools work.

### Track B — full GUI editor (for PIE + live backend end-to-end)
`UnrealEditor.exe <proj> -unattended -nopause -nosplash -RenderOffScreen
-stdout` (NOT `-Cmd`, NO `-run=`, NO `-nullrhi`). `-unattended` defaults the
startup modal; the plugin's `FLiveServer` auto-starts at module init and
publishes `Saved/bp-reader-live.json`, so the `live`/`auto` backend connects
exactly as to a human-run editor — **no plugin wire change needed**. This is
the only path that sustains a real **PIE** world and exercises the live
TCP+auth+game-thread path end-to-end.

- **Cost:** Slate needs an **interactive desktop session** (autologon / RDP-kept
  session); a service-session runner has no desktop. This is the biggest
  repeatability risk and why Track B is "human-supervised CI" until an
  autologon box exists.
- Belt-and-suspenders modal gating: also pass `-RUNNINGUNATTENDEDSCRIPT` and (in
  `FBlueprintReaderEditorModule::StartupModule`, behind a `BP_READER_GUI_
  AUTOMATION=1` env gate) set `GIsRunningUnattendedScript = true` so bespoke
  non-`FMessageDialog` Slate dialogs also auto-default.

## Phased plan

**Phase 0 — local spike (cheapest, settles the make-or-break, ~1–2h):**
1. Copy `start-daemon.ps1` → `start-render-daemon.ps1`: replace `-nullrhi` with
   `-AllowCommandletRendering` (use the box's GPU) — optionally `-RenderOffScreen
   -dx12 -WARP` to test the GPU-free path separately.
2. Drive `take_viewport_screenshot` / `take_screenshot` / `set_camera_transform`
   / `build_lighting` against a real loaded map. Inspect: `captured:true` +
   a non-empty PNG on disk? That single result decides whether Track A is real.
   ⚠️ This box has TDR-crashed a GPU live editor mid-batch before (see memory);
   run the spike deliberately, not in an unattended loop.
3. Also spike Track B: `UnrealEditor.exe <proj> -unattended -nopause -nosplash`
   → does `bp-reader-live.json` appear + the live socket answer? Note any
   non-`FMessageDialog` dialog that still wedges.

**Phase 1 — Track A in CI:** add a job to the self-hosted `ue5` workflow that
rebuilds the server (stale-exe gives false results), starts the render daemon,
waits on the handshake, runs `BlueprintReaderMcpTests.exe` with
`BP_READER_BACKEND=commandlet BP_READER_SMOKE_ALL=1 BP_READER_SMOKE_RENDER=1`,
and uploads the captured PNGs + doctest log as artifacts (+ golden-image diff).

**Phase 2 — Track B (PIE + live):** on an autologon GPU box, launch the GUI
editor, run the same smoke against `BP_READER_BACKEND=live`, gate PIE behind
`BP_READER_SMOKE_PIE=1`.

## Code changes required (small)
- `test_tool_smoke_live.cpp`: (1) pick `SocketBlueprintReader` when
  `BP_READER_BACKEND=live` (it currently hardcodes `CommandletBlueprintReader`);
  (2) move the render/capture tools out of registration-only checking behind a
  new `BP_READER_SMOKE_RENDER` flag so they actually run and assert
  `captured/moved:true` when the editor is render-capable; gate PIE behind
  `BP_READER_SMOKE_PIE`.
- `Saved/start-render-daemon.ps1` (new): the Track-A launch variant.
- (Track B only) the optional `BP_READER_GUI_AUTOMATION` startup gate in the
  editor module.
- The render tools themselves need **no** change — they already gate on
  `CanEverRender()`, so they proceed automatically once an RHI is present.

## Verdict (fidelity-first)
- **For accuracy to active-editor usage, Track B is the real answer** for every
  active-editor-state-dependent tool (viewport/selection/camera/PIE/open-editor/
  world). The test must drive a real GUI editor — with a map loaded and a live
  viewport — through the `live` backend, the exact setup a human has when using
  the MCP server interactively, and put the editor in a representative state
  before asserting. Track B needs an interactive desktop session (autologon), so
  it starts human-supervised and graduates to CI once an autologon GPU runner
  exists.
- **Track A is a cheaper *partial* measure, not a fidelity substitute.** A
  render-capable headless daemon (`-AllowCommandletRendering`) is faithful only
  for render captures that don't depend on the active viewport/selection
  (`build_lighting`, an explicitly-opened-scene HighResShot). Treating its
  screenshot/viewport/selection responses as representative of active use would
  be a false positive — so the Phase-0 spike must record *which* tools it can
  faithfully cover and which still require Track B.
- Net: the BP/asset (state-independent) surface is already faithfully covered
  headless; the render+interactive surface needs the real, in-use GUI editor to
  match how the tools actually respond to a person using it.
