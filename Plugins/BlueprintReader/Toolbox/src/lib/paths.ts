// Env paths resolved by the main process (Windows-specific).
export interface EnvPaths {
  userProfile: string;  // %USERPROFILE%  e.g. C:\Users\Alice
  appData: string;      // %APPDATA%       e.g. C:\Users\Alice\AppData\Roaming
}

export type ProviderScope = 'project' | 'global' | 'manual';

export interface ProviderConfig {
  id: string;
  label: string;
  description: string;
  scope: ProviderScope;
  // Returns the full config file path. Global providers use env for %APPDATA%/%USERPROFILE%.
  configPath: (projectDir: string, env: EnvPaths) => string;
  configKey: string[];
  format: 'json' | 'toml' | 'manual';
  // When set, the Toolbox writes this content directly instead of calling the script.
  // Used for providers not yet supported by BlueprintReaderMcp.exe config --client=X.
  buildConfig?: (exePath: string) => string;
}

export const PROVIDERS: ProviderConfig[] = [
  // ── Project-scoped (written into the project tree) ──────────────────────
  {
    id: 'ClaudeCode',
    label: 'Claude Code',
    description: 'Anthropic CLI — reads .mcp.json at the project root',
    scope: 'project',
    configPath: (d) => `${d}\\.mcp.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Cursor',
    label: 'Cursor',
    description: 'Cursor IDE — project-level config in .cursor/mcp.json',
    scope: 'project',
    configPath: (d) => `${d}\\.cursor\\mcp.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'VSCode',
    label: 'VS Code / GitHub Copilot',
    description: 'VS Code Copilot extension — .vscode/mcp.json uses "servers" key (not mcpServers)',
    scope: 'project',
    configPath: (d) => `${d}\\.vscode\\mcp.json`,
    configKey: ['servers', 'bp-reader'],
    format: 'json',
  },

  // ── Global/user-scoped (written to %APPDATA% or %USERPROFILE%) ──────────
  {
    id: 'ClaudeDesktop',
    label: 'Claude Desktop',
    description: 'Anthropic desktop app — global config at %APPDATA%\\Claude\\claude_desktop_config.json',
    scope: 'global',
    configPath: (_, env) => `${env.appData}\\Claude\\claude_desktop_config.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Windsurf',
    label: 'Windsurf',
    description: 'Codeium Windsurf IDE — global config at %USERPROFILE%\\.codeium\\windsurf\\mcp_config.json',
    scope: 'global',
    configPath: (_, env) => `${env.userProfile}\\.codeium\\windsurf\\mcp_config.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Gemini',
    label: 'Gemini CLI',
    description: 'Google Gemini CLI — global config at %USERPROFILE%\\.gemini\\settings.json',
    scope: 'global',
    configPath: (_, env) => `${env.userProfile}\\.gemini\\settings.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Codex',
    label: 'Codex CLI',
    description: 'OpenAI Codex CLI — global TOML config at %USERPROFILE%\\.codex\\config.toml',
    scope: 'global',
    configPath: (_, env) => `${env.userProfile}\\.codex\\config.toml`,
    configKey: ['mcp_servers', 'bp-reader'],
    format: 'toml',
  },
  {
    id: 'CopilotCLI',
    label: 'GitHub Copilot CLI',
    description: 'gh copilot CLI extension — global config at %USERPROFILE%\\.copilot\\mcp-config.json',
    scope: 'global',
    configPath: (_, env) => `${env.userProfile}\\.copilot\\mcp-config.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
    // gh copilot uses "type": "local" instead of "type": "stdio"
    buildConfig: (exePath) => JSON.stringify({
      mcpServers: {
        'bp-reader': {
          type: 'local',
          command: exePath,
          args: [],
          env: { BP_READER_PREWARM: '1', BP_READER_EDITOR_ARGS: '-EnableAllPlugins' },
        },
      },
    }, null, 2),
  },

  // ── Manual (no file-based config — must be set in the IDE's settings UI) ─
  {
    id: 'JetBrains',
    label: 'JetBrains IDEs (Rider / IntelliJ)',
    description: 'JetBrains AI plugin uses a GUI-only config — go to Settings → Tools → AI Assistant → Model Context Protocol (MCP)',
    scope: 'manual',
    configPath: () => '',
    configKey: [],
    format: 'manual',
  },
];

export type ProviderStatus = 'configured' | 'stale' | 'missing';

function getNestedKey(obj: Record<string, unknown>, keys: string[]): unknown {
  let cur: unknown = obj;
  for (const k of keys) {
    if (typeof cur !== 'object' || cur === null) return undefined;
    cur = (cur as Record<string, unknown>)[k];
  }
  return cur;
}

export function getJsonProviderStatus(
  content: string | null,
  keys: string[],
  exePath: string
): ProviderStatus {
  if (!content) return 'missing';
  let cfg: Record<string, unknown>;
  try {
    cfg = JSON.parse(content) as Record<string, unknown>;
  } catch {
    return 'missing';
  }
  const entry = getNestedKey(cfg, keys);
  if (!entry || typeof entry !== 'object') return 'missing';
  const e = entry as Record<string, unknown>;
  const cmd = e['command'];
  if (!cmd) return 'missing';
  const normalize = (s: string) => s.replace(/\\/g, '/').toLowerCase();
  if (normalize(String(cmd)) !== normalize(exePath)) return 'stale';
  return 'configured';
}

export function getTomlProviderStatus(content: string | null): ProviderStatus {
  if (!content) return 'missing';
  if (content.includes('[mcp_servers.bp-reader]') || content.includes('["mcp_servers"]["bp-reader"]')) {
    return 'configured';
  }
  return 'missing';
}

export function normalizePathForCompare(p: string): string {
  return p.replace(/\\/g, '/').toLowerCase();
}

// ── Cross-page project state ──────────────────────────────────────────────

const STORAGE_KEY = 'bpr-uproject';

export function storeUproject(uproject: string): void {
  if (uproject) localStorage.setItem(STORAGE_KEY, uproject);
}

export function loadUproject(): string {
  return localStorage.getItem(STORAGE_KEY) ?? '';
}

export function uprojectToPluginDir(uproject: string): string {
  if (!uproject) return '';
  const projectDir = uproject.replace(/[/\\][^/\\]+\.uproject$/, '');
  return `${projectDir}\\Plugins\\BlueprintReader`;
}

export function uprojectToExePath(uproject: string): string {
  return uprojectToPluginDir(uproject) + '\\Binaries\\Win64\\BlueprintReaderMcp.exe';
}
