import { useState, useEffect, useCallback, useRef } from 'react';
import { bridge } from '../lib/bridge';
import StatusBadge, { type Status } from '../components/StatusBadge';
import LogStream from '../components/LogStream';
import {
  PROVIDERS,
  getJsonProviderStatus,
  getTomlProviderStatus,
  mergeServerEntry,
  buildServerEntry,
  type ProviderConfig,
  type ProviderStatus,
  type EnvPaths,
  loadUproject,
  uprojectToPluginDir,
  uprojectToExePath,
} from '../lib/paths';
import { loadEnvOverrides } from '../lib/settings';

interface ProviderState {
  status: Status;
}

// Map provider id → the --client flag BlueprintReaderMcp.exe config accepts.
// Providers not listed here are configured by the Toolbox directly (buildConfig).
const SCRIPT_CLIENT_MAP: Record<string, string> = {
  ClaudeCode:    'claude-code',
  ClaudeDesktop: 'claude-desktop',
  Cursor:        'cursor',
  Windsurf:      'windsurf',
  VSCode:        'copilot',
  Gemini:        'gemini',
  Codex:         'codex',
};

const DEFAULT_ENV: EnvPaths = { userProfile: '', appData: '' };

export default function Providers() {
  const [uproject, setUproject] = useState(() => loadUproject());
  const [envPaths, setEnvPaths] = useState<EnvPaths>(DEFAULT_ENV);
  const [states, setStates] = useState<Record<string, ProviderState>>(
    Object.fromEntries(PROVIDERS.map((p) => [p.id, { status: 'loading' as Status }]))
  );
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [assetsInstalled, setAssetsInstalled] = useState<boolean | null>(null);
  const [selected, setSelected] = useState<string | null>(null);

  const projectDir = uproject ? uproject.replace(/[/\\][^/\\]+\.uproject$/, '') : '';
  const pluginDir  = uprojectToPluginDir(uproject);
  const exePath    = uprojectToExePath(uproject);

  // TBX-R9: mounted guard + unsub-on-unmount for the script-log listener.
  const mountedRef = useRef(true);
  const unsubRef = useRef<(() => void) | null>(null);
  useEffect(() => () => { mountedRef.current = false; unsubRef.current?.(); unsubRef.current = null; }, []);

  const appendLog = useCallback((line: string) => {
    if (mountedRef.current) setLogs((prev) => [...prev, line]);
  }, []);

  useEffect(() => {
    bridge.getEnvPaths().then(setEnvPaths).catch(() => {});
    bridge.getPaths().then(async (p) => {
      const saved = loadUproject() || p.uproject;
      if (saved && saved !== uproject) setUproject(saved);
    });
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!projectDir || !envPaths.userProfile) return;
    refreshStatuses();
    checkAssetsInstalled();
  }, [projectDir, exePath, envPaths]); // eslint-disable-line react-hooks/exhaustive-deps

  async function refreshStatuses() {
    const newStates: Record<string, ProviderState> = {};
    for (const provider of PROVIDERS) {
      if (provider.scope === 'manual') {
        newStates[provider.id] = { status: 'missing' };
        continue;
      }
      const configPath = provider.configPath(projectDir, envPaths);
      const content = await bridge.readFile(configPath);
      let status: ProviderStatus;
      if (provider.format === 'toml') {
        status = getTomlProviderStatus(content, exePath);
      } else {
        status = getJsonProviderStatus(content, provider.configKey, exePath);
      }
      newStates[provider.id] = { status };
    }
    setStates(newStates);
  }

  // TBX-R8: the deploy writes several asset targets — probe all the concrete
  // ones (AGENTS.md, the Copilot instructions, and a representative Claude skill)
  // and only report "installed" when they're all present, so a half-deploy isn't
  // shown as complete.
  async function checkAssetsInstalled() {
    if (!projectDir) return;
    const targets = [
      `${projectDir}/AGENTS.md`,
      `${projectDir}/.github/copilot-instructions.md`,
      `${projectDir}/.claude/skills/bp-reader/SKILL.md`,
    ];
    const present = await Promise.all(targets.map((p) => bridge.readFile(p).then((c) => c !== null).catch(() => false)));
    if (mountedRef.current) setAssetsInstalled(present.every(Boolean));
  }

  // Write one provider's config. Appends to the log but does NOT clear it /
  // toggle running / refresh — the callers orchestrate that, so Configure All
  // can show every provider's result (TBX-F9) instead of clobbering the log.
  async function configureOne(provider: ProviderConfig, settingsEnv: Record<string, string>): Promise<boolean> {
    if (provider.format === 'json') {
      // Toolbox writes the config directly, MERGING our entry under configKey so
      // sibling MCP servers in a shared/global file are preserved — and injecting
      // the user's Settings env so "Save" on the Settings page actually applies.
      const configPath = provider.configPath(projectDir, envPaths);
      try {
        const existing = await bridge.readFile(configPath);
        // Pin BP_READER_PROJECT like the old Generate-ClientConfig.ps1 did, so a
        // server exe outside the project tree still finds the project (Settings
        // overrides win). Backend defaults to auto once a project is known.
        const env = uproject ? { BP_READER_PROJECT: uproject, ...settingsEnv } : settingsEnv;
        const merged = mergeServerEntry(existing, provider.configKey,
          buildServerEntry(provider, exePath, env));
        await bridge.writeFile(configPath, merged);
        appendLog(`[ok] ${provider.label} → ${configPath}`
          + (Object.keys(settingsEnv).length ? ` (+${Object.keys(settingsEnv).length} Settings override(s))` : ''));
        return true;
      } catch (e) {
        appendLog(`[error] ${provider.label}: ${e instanceof Error ? e.message : String(e)}`);
        return false;
      }
    }
    if (provider.format === 'toml') {
      // Codex (TOML) is still generated by the script. NOTE: Settings-env
      // injection for the TOML path is TBX-F1 follow-up (script -ServerEnvJson).
      const scriptPath = `${pluginDir}\\Scripts\\Generate-ClientConfig.ps1`;
      const present = await bridge.readFile(scriptPath);
      if (present === null) {
        appendLog(`[error] ${provider.label}: ${scriptPath} not found — install the plugin into this project first (Install tab).`);
        return false;
      }
      const clientFlag = SCRIPT_CLIENT_MAP[provider.id] ?? provider.id;
      return (await bridge.runScript(scriptPath, ['-Client', clientFlag])) === 0;
    }
    return false;
  }

  async function configureProvider(providerId: string) {
    setLogs([]);
    setRunning(true);
    unsubRef.current = bridge.onScriptLog(appendLog);
    const provider = PROVIDERS.find((p) => p.id === providerId)!;
    await configureOne(provider, loadEnvOverrides());
    unsubRef.current?.(); unsubRef.current = null;
    setRunning(false);
    await refreshStatuses();
  }

  async function configureAll() {
    setLogs([]);
    setRunning(true);
    unsubRef.current = bridge.onScriptLog(appendLog);
    const settingsEnv = loadEnvOverrides();
    const targets = PROVIDERS.filter((p) => p.scope !== 'manual');
    let ok = 0;
    for (const p of targets) {
      appendLog(`── ${p.label} ──`);
      if (await configureOne(p, settingsEnv)) ok++;
    }
    appendLog(`\nConfigured ${ok}/${targets.length} provider(s).`);
    unsubRef.current?.(); unsubRef.current = null;
    setRunning(false);
    await refreshStatuses();
  }

  async function deployAssets() {
    setLogs([]);
    setRunning(true);
    unsubRef.current = bridge.onScriptLog(appendLog);
    // Works whether or not the plugin is already mounted: the IPC runs the
    // in-project Install-ClaudeAssets.ps1 if present, else downloads the latest
    // release and runs the extracted copy.
    const res = await bridge.deployAssets({ projectDir });
    unsubRef.current?.(); unsubRef.current = null;
    if (!res.ok && res.error) appendLog(`[error] ${res.error}`);
    setRunning(false);
    checkAssetsInstalled();
  }

  function openConfigFile(path: string) {
    if (path) bridge.openExternal(`file:///${path.replace(/\\/g, '/')}`);
  }

  if (!uproject) {
    return (
      <div className="p-6 max-w-2xl">
        <h1 className="text-xl font-semibold text-white mb-1">AI Providers</h1>
        <div className="mt-6 p-4 bg-ue-accent/10 border border-ue-accent/30 rounded text-sm text-gray-300">
          No project configured yet. Go to the <strong className="text-white">Install</strong> tab,
          select your <code className="text-gray-300">.uproject</code> file, then return here.
        </div>
      </div>
    );
  }

  const selectedProvider = selected ? PROVIDERS.find((p) => p.id === selected) : null;

  return (
    <div className="p-6 max-w-4xl">
      <div className="flex items-center justify-between mb-4">
        <div>
          <h1 className="text-xl font-semibold text-white mb-1">AI Providers</h1>
          <p className="text-gray-500 text-sm">Configure the MCP connection for each AI client.</p>
        </div>
        <button
          onClick={configureAll}
          disabled={running}
          className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white"
        >
          Configure All
        </button>
      </div>

      {/* Provider grid */}
      <div className="grid grid-cols-1 gap-2 mb-6">
        {PROVIDERS.map((provider) => {
          const state = states[provider.id] ?? { status: 'loading' as Status };
          const configPath = provider.scope !== 'manual'
            ? provider.configPath(projectDir, envPaths)
            : '';
          const isSelected = selected === provider.id;

          return (
            <div key={provider.id} className={`border rounded transition-colors ${isSelected ? 'border-ue-accent/60 bg-ue-panel2' : 'border-ue-border bg-ue-panel'}`}>
              {/* Row */}
              <div
                className="p-3 flex items-center gap-4 cursor-pointer hover:bg-white/5"
                onClick={() => setSelected(isSelected ? null : provider.id)}
              >
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 mb-0.5">
                    <span className="text-sm font-medium text-white">{provider.label}</span>
                    {provider.scope === 'manual'
                      ? <span className="text-xs text-gray-500 bg-ue-dark px-1.5 py-0.5 rounded">GUI only</span>
                      : <StatusBadge status={state.status} />}
                  </div>
                  <div className="text-xs text-gray-500 truncate">
                    {configPath || provider.description}
                  </div>
                </div>
                <div className="flex gap-2 flex-shrink-0 items-center">
                  {configPath && (
                    <button
                      onClick={(e) => { e.stopPropagation(); openConfigFile(configPath); }}
                      className="px-2.5 py-1 bg-ue-dark border border-ue-border rounded text-xs hover:bg-white/10 text-gray-400"
                    >
                      Open file
                    </button>
                  )}
                  {provider.scope !== 'manual' && (
                    <button
                      onClick={(e) => { e.stopPropagation(); configureProvider(provider.id); }}
                      disabled={running}
                      className="px-3 py-1 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-xs text-white"
                    >
                      Configure
                    </button>
                  )}
                  <span className={`text-gray-500 text-xs transition-transform ${isSelected ? 'rotate-180' : ''}`}>▾</span>
                </div>
              </div>

              {/* Expanded detail */}
              {isSelected && selectedProvider && (
                <div className="px-4 pb-4 border-t border-ue-border/50 pt-3">
                  <p className="text-xs text-gray-400 mb-3">{selectedProvider.description}</p>

                  {selectedProvider.scope === 'manual' ? (
                    <div className="bg-ue-dark rounded p-3 text-xs text-gray-300 space-y-1.5">
                      <div className="font-medium text-white mb-1">Setup instructions</div>
                      <div>1. Open your JetBrains IDE (Rider, IntelliJ, etc.)</div>
                      <div>2. Go to <strong>Settings → Tools → AI Assistant → Model Context Protocol (MCP)</strong></div>
                      <div>3. Click <strong>+</strong> and set Command to:</div>
                      <code className="block bg-black/40 rounded px-2 py-1 text-gray-300 mt-1 break-all">{exePath}</code>
                      <div className="mt-1">4. Add env var <code className="text-gray-300">BP_READER_PREWARM=1</code></div>
                    </div>
                  ) : selectedProvider.scope === 'global' ? (
                    <div className="text-xs text-gray-400">
                      <span className="text-gray-500">Writes to: </span>
                      <span className="text-gray-200 font-mono break-all">{configPath}</span>
                      <div className="mt-1 text-gray-500">This is a user-global config — applies across all projects.</div>
                    </div>
                  ) : (
                    <div className="text-xs text-gray-400">
                      <span className="text-gray-500">Writes to: </span>
                      <span className="text-gray-200 font-mono break-all">{configPath}</span>
                      <div className="mt-1 text-gray-500">Project-scoped — commit this file to share the setup with your team.</div>
                    </div>
                  )}
                </div>
              )}
            </div>
          );
        })}
      </div>

      {/* Log output */}
      {logs.length > 0 && (
        <div className="mb-6">
          <label className="block text-xs text-gray-500 mb-1">Output</label>
          <LogStream lines={logs} maxHeight="180px" />
        </div>
      )}

      {/* Skills & Agents */}
      <div className="border-t border-ue-border pt-5">
        <div className="flex items-center justify-between mb-3">
          <div>
            <h2 className="text-base font-medium text-white mb-0.5">Skills & Agents</h2>
            <p className="text-gray-500 text-xs">
              Each AI client reads its guidance from a different file, so one deploy writes them all:
              <code className="text-gray-400"> AGENTS.md</code> (the cross-agent standard — Codex, Cursor, Gemini, Aider, Jules),
              <code className="text-gray-400"> .github/copilot-instructions.md</code> (GitHub Copilot, incl. the JetBrains/Rider plugin),
              and <code className="text-gray-400"> .claude/skills</code> + <code className="text-gray-400">.claude/agents</code> (Claude Code).
              Existing content in those files is section-merged, not overwritten.
            </p>
          </div>
          <div className="flex items-center gap-3">
            {assetsInstalled !== null && (
              <StatusBadge status={assetsInstalled ? 'configured' : 'missing'} />
            )}
            <button
              onClick={deployAssets}
              disabled={running}
              className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white"
            >
              Deploy Assets
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
