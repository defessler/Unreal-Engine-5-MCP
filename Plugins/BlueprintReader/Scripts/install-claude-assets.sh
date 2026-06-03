#!/usr/bin/env bash
#
# install-claude-assets.sh
#
# Copies the bp-reader plugin's Claude skills + agents into the parent
# project's .claude/ directory, AND deploys the plugin's AGENTS.md to the
# project root + .github/copilot-instructions.md (cross-AI guidance for
# Codex / Cursor / Aider / GitHub Copilot). Idempotent — re-running
# overwrites plugin-managed targets; a marker guard skips a consumer's own
# AGENTS.md / copilot file (pass --force to overwrite). Parity with
# Install-ClaudeAssets.ps1 so non-Windows / CI users get the same surface.
#
# Source of truth: Plugins/BlueprintReader/Claude/ + Plugins/BlueprintReader/AGENTS.md
# Deploy target:   <project-root>/.claude/, <project-root>/AGENTS.md,
#                  <project-root>/.github/copilot-instructions.md
#
# Usage:
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh --dry-run
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh --force
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh --project-root /path/to/project

set -euo pipefail

DRY_RUN=0
FORCE=0
PROJECT_ROOT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --force) FORCE=1; shift ;;
        --project-root) PROJECT_ROOT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- Locate source + target -----------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_ROOT="$(dirname "$SCRIPT_DIR")"
CLAUDE_SRC="$PLUGIN_ROOT/Claude"

if [[ ! -d "$CLAUDE_SRC" ]]; then
    echo "Plugin Claude/ folder not found at $CLAUDE_SRC" >&2
    exit 1
fi

if [[ -z "$PROJECT_ROOT" ]]; then
    # Walk up from plugin root looking for a *.uproject.
    candidate="$PLUGIN_ROOT"
    for _ in 1 2 3 4 5; do
        candidate="$(dirname "$candidate")"
        if compgen -G "$candidate"/*.uproject > /dev/null; then
            PROJECT_ROOT="$candidate"
            break
        fi
    done
    if [[ -z "$PROJECT_ROOT" ]]; then
        echo "Could not auto-detect project root (no .uproject found walking up from plugin)." >&2
        echo "Pass --project-root <path> explicitly." >&2
        exit 1
    fi
fi

CLAUDE_DST="$PROJECT_ROOT/.claude"

echo "bp-reader Claude assets installer"
echo "  Source:  $CLAUDE_SRC"
echo "  Target:  $CLAUDE_DST"
if [[ $DRY_RUN -eq 1 ]]; then
    echo "  DryRun:  yes (no files will change)"
fi

# ---- Plan + execute -------------------------------------------------

installed=0

# Skills: directory copy. Each subdir under Claude/skills/ becomes
# a subdir under .claude/skills/.
if [[ -d "$CLAUDE_SRC/skills" ]]; then
    for skill_dir in "$CLAUDE_SRC/skills"/*/; do
        [[ -d "$skill_dir" ]] || continue
        skill_name="$(basename "$skill_dir")"
        target="$CLAUDE_DST/skills/$skill_name"
        action="CREATE"
        [[ -e "$target" ]] && action="UPDATE"
        echo "  $action skill: $target"
        if [[ $DRY_RUN -eq 0 ]]; then
            mkdir -p "$CLAUDE_DST/skills"
            rm -rf "$target"
            cp -R "$skill_dir" "$target"
        fi
        installed=$((installed + 1))
    done
fi

# Agents: file copy. Each *.md under Claude/agents/ goes into
# .claude/agents/.
if [[ -d "$CLAUDE_SRC/agents" ]]; then
    for agent_file in "$CLAUDE_SRC/agents"/*.md; do
        [[ -f "$agent_file" ]] || continue
        agent_name="$(basename "$agent_file")"
        target="$CLAUDE_DST/agents/$agent_name"
        action="CREATE"
        [[ -e "$target" ]] && action="UPDATE"
        echo "  $action agent: $target"
        if [[ $DRY_RUN -eq 0 ]]; then
            mkdir -p "$CLAUDE_DST/agents"
            cp -f "$agent_file" "$target"
        fi
        installed=$((installed + 1))
    done
fi

# Cross-AI guidance: ONE portable source (the plugin's AGENTS.md) deployed to
# the locations each assistant family auto-discovers — <root>/AGENTS.md
# (Codex / Cursor / Aider) and <root>/.github/copilot-instructions.md (GitHub
# Copilot). Same content, single source, no drift. A marker guard avoids
# clobbering a consumer project's own hand-written file.
AGENTS_SRC="$PLUGIN_ROOT/AGENTS.md"
if [[ -f "$AGENTS_SRC" ]]; then
    for dst in "$PROJECT_ROOT/AGENTS.md" "$PROJECT_ROOT/.github/copilot-instructions.md"; do
        if [[ -e "$dst" && $FORCE -eq 0 ]] && ! grep -q "BlueprintReader MCP" "$dst" 2>/dev/null; then
            echo "  SKIP   cross-ai: $dst (consumer-owned; re-run with --force to overwrite)"
            continue
        fi
        action="CREATE"
        [[ -e "$dst" ]] && action="UPDATE"
        echo "  $action cross-ai: $dst"
        if [[ $DRY_RUN -eq 0 ]]; then
            mkdir -p "$(dirname "$dst")"
            cp -f "$AGENTS_SRC" "$dst"
        fi
        installed=$((installed + 1))
    done
fi

if [[ $installed -eq 0 ]]; then
    echo "Nothing to install — Claude/ folder is empty." >&2
    exit 0
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo "Dry run complete. Re-run without --dry-run to apply."
else
    echo "Installed $installed item(s). Restart Claude Code to pick up the new skill manifests."
fi
