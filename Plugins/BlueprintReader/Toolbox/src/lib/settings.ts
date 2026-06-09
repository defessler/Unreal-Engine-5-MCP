// The single source of truth for the env-override block the Settings page edits.
// Persisted in localStorage and READ BY Providers when it writes each MCP client
// config — so "Save" on the Settings page actually applies (TBX-F1), instead of
// writing a dead key nothing reads back.
const KEY = 'bpr-env-overrides';

export function loadEnvOverrides(): Record<string, string> {
  try {
    const s = localStorage.getItem(KEY);
    if (!s) return {};
    const v = JSON.parse(s);
    return v && typeof v === 'object' ? (v as Record<string, string>) : {};
  } catch {
    // Corrupt storage must not crash the page (TBX-R9) — start clean.
    return {};
  }
}

export function saveEnvOverrides(env: Record<string, string>): void {
  localStorage.setItem(KEY, JSON.stringify(env));
}
