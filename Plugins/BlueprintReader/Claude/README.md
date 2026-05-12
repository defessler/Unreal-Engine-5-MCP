# bp-reader plugin — Claude skills & agents

This folder ships **with the plugin** so the skills and agents that
teach Claude (and compatible MCP clients) how to use bp-reader stay
in sync with the code. When you drop a fresh copy of
`Plugins/BlueprintReader/` into your UE project, you also pick up the
latest skill manifests here.

## Layout

```
Plugins/BlueprintReader/
└── Claude/                       ← source of truth (this folder)
    ├── README.md                 ← (you are here)
    ├── agents/
    │   └── bp-audit.md           ← read-only structural audit agent
    └── skills/
        ├── bp-reader/SKILL.md    ← general MCP usage guide
        ├── bp-batches/SKILL.md   ← apply_ops / compile_function
        ├── bp-cpp/SKILL.md       ← BP ↔ C++ transpile workflows
        └── bp-debug/SKILL.md     ← failure triage + recovery
```

Each `SKILL.md` has YAML frontmatter (`name`, `description`) that
Claude Code uses for auto-activation. Each `agents/*.md` is a
subagent manifest.

## Installing into a project

Claude Code discovers skills + agents from `<project-root>/.claude/`.
This plugin ships them under `Claude/` (capitalized, per UE folder
convention) so the install step is a one-shot copy:

```pwsh
# Windows / PowerShell
pwsh Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1
```

```bash
# macOS / Linux
./Plugins/BlueprintReader/Scripts/install-claude-assets.sh
```

The script:
- Auto-detects the project root by walking up from the plugin looking
  for a sibling `*.uproject`. Override with `-ProjectRoot <path>` /
  `--project-root <path>` if needed.
- Copies every `Claude/skills/*` into `<project>/.claude/skills/`.
- Copies every `Claude/agents/*.md` into `<project>/.claude/agents/`.
- Is idempotent — re-running overwrites whatever's there with the
  plugin's current version.
- Leaves anything else under `.claude/` alone (other skills, settings).
- Supports `-DryRun` / `--dry-run` to preview without writing.

Restart Claude Code after installing so it picks up the new manifests.

## Why ship them with the plugin

The skills describe **the wire shape this version of the plugin
emits** (`linked_to` arrays on pins, `meta.castBroken` on broken
casts, `find_node` matching against `meta.targetFunction`, cascade
diagnostics on `apply_ops`, etc.). Keeping them next to the code
means:

- A user who pulls the latest plugin gets matching skills automatically.
- A user pinned to an older plugin gets the skill version that
  actually describes its wire shape — no false promises from the
  current main-branch docs.
- Forking / vendoring the plugin into another repo brings the
  skills along.

## Editing

This folder is the source of truth. Edit `Claude/skills/<name>/SKILL.md`
or `Claude/agents/<name>.md` here, then run the install script to
deploy. The committed `.claude/` at the project root in this
development repo is just the deployed copy — keep it in sync by
re-running the script after any edit.

If you're working on a fork of the plugin for distribution, the
shipped layout is:
- `Plugins/BlueprintReader/Claude/` — content to publish.
- `Plugins/BlueprintReader/Scripts/Install-ClaudeAssets.ps1` and
  `install-claude-assets.sh` — what users run after `git pull`.

## Versioning

Skills are tied to the plugin version. When the wire format changes,
update both the code AND the relevant skill file in the same commit
so docs and reality don't drift. The skills mention concrete wire
shapes (`graph_name`, `linked_to`, `castBroken`, …) so checking
"does the skill describe what the code actually does?" is mechanical
during review.
