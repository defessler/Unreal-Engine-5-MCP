#!/usr/bin/env bash
#
# install-claude-assets.sh
#
# Copies the bp-reader plugin's Claude skills + agents into the parent
# project's .claude/ directory so Claude Code (and compatible clients)
# discover them. Idempotent — re-running overwrites the target.
#
# Source of truth: Plugins/BlueprintReader/Claude/
# Deploy target:   <project-root>/.claude/
#
# Usage:
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh --dry-run
#   ./Plugins/BlueprintReader/Scripts/install-claude-assets.sh --project-root /path/to/project

set -euo pipefail

DRY_RUN=0
PROJECT_ROOT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
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

if [[ $installed -eq 0 ]]; then
    echo "Nothing to install — Claude/ folder is empty." >&2
    exit 0
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo "Dry run complete. Re-run without --dry-run to apply."
else
    echo "Installed $installed item(s). Restart Claude Code to pick up the new skill manifests."
fi
