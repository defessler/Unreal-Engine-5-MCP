export interface ProviderConfig {
  id: string;
  label: string;
  configPath: (projectDir: string) => string;
  configKey: string[];
  format: 'json' | 'toml';
}

export const PROVIDERS: ProviderConfig[] = [
  {
    id: 'ClaudeCode',
    label: 'Claude Code',
    configPath: (d) => `${d}/.mcp.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Cursor',
    label: 'Cursor',
    configPath: (d) => `${d}/.cursor/mcp.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'VSCode',
    label: 'VS Code / Copilot',
    configPath: (d) => `${d}/.vscode/mcp.json`,
    configKey: ['servers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Rider',
    label: 'JetBrains Rider',
    configPath: (d) => `${d}/.idea/mcp.json`,
    configKey: ['servers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Gemini',
    label: 'Gemini',
    configPath: (d) => `${d}/.gemini/settings.json`,
    configKey: ['mcpServers', 'bp-reader'],
    format: 'json',
  },
  {
    id: 'Codex',
    label: 'Codex',
    configPath: (d) => `${d}/.codex/config.toml`,
    configKey: ['mcp_servers', 'bp-reader'],
    format: 'toml',
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
  // Normalize path separators for comparison
  const normalize = (s: string) => s.replace(/\\/g, '/').toLowerCase();
  if (normalize(String(cmd)) !== normalize(exePath)) return 'stale';
  return 'configured';
}

export function getTomlProviderStatus(content: string | null): ProviderStatus {
  if (!content) return 'missing';
  // Minimal check: look for [mcp_servers.bp-reader] section header
  if (content.includes('[mcp_servers.bp-reader]') || content.includes('["mcp_servers"]["bp-reader"]')) {
    return 'configured';
  }
  return 'missing';
}

// Normalize path separators for comparison (case-insensitive, forward slashes)
export function normalizePathForCompare(p: string): string {
  return p.replace(/\\/g, '/').toLowerCase();
}
