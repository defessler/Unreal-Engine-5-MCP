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
  // When set, the Toolbox writes this provider's config itself (instead of
  // calling Generate-ClientConfig.ps1) by MERGING this entry under configKey
  // into the existing file — so sibling MCP servers in a shared/global config
  // are preserved. Returns just the bp-reader server entry object.
  serverEntry?: (exePath: string) => Record<string, unknown>;
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
    serverEntry: (exePath) => ({
      type: 'local',
      command: exePath,
      args: [],
      env: { BP_READER_PREWARM: '1', BP_READER_EDITOR_ARGS: '-EnableAllPlugins' },
    }),
  },

  {
    id: 'JetBrains',
    label: 'JetBrains Copilot (Rider / IntelliJ)',
    description: 'GitHub Copilot plugin for JetBrains — global config at %APPDATA%\\github-copilot\\intellij\\mcp.json (uses the "servers" key + "type":"stdio", like VS Code Copilot)',
    scope: 'global',
    configPath: (_, env) => `${env.appData}\\github-copilot\\intellij\\mcp.json`,
    configKey: ['servers', 'bp-reader'],
    format: 'json',
    serverEntry: (exePath) => ({
      type: 'stdio',
      command: exePath,
      args: [],
      env: { BP_READER_PREWARM: '1', BP_READER_EDITOR_ARGS: '-EnableAllPlugins' },
    }),
  },
];

// Merge a server entry under configKey into an existing JSON config file's text
// (preserving sibling servers). Returns the new file text. Used by providers
// with a serverEntry so a shared/global config isn't clobbered.
export function mergeServerEntry(
  existing: string | null,
  keys: string[],
  entry: Record<string, unknown>,
): string {
  let root: Record<string, unknown> = {};
  if (existing) { try { root = JSON.parse(existing) as Record<string, unknown>; } catch { root = {}; } }
  let cur: Record<string, unknown> = root;
  for (let i = 0; i < keys.length - 1; i++) {
    const k = keys[i];
    if (typeof cur[k] !== 'object' || cur[k] === null) cur[k] = {};
    cur = cur[k] as Record<string, unknown>;
  }
  cur[keys[keys.length - 1]] = entry;
  return JSON.stringify(root, null, 2);
}

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
