import { useState, useEffect, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import StatusBadge, { type Status } from '../components/StatusBadge';
import LogStream from '../components/LogStream';
import {
  PROVIDERS,
  getJsonProviderStatus,
  getTomlProviderStatus,
  type ProviderStatus,
} from '../lib/paths';

interface ProviderState {
  status: Status;
}

export default function Providers() {
  const [projectDir, setProjectDir] = useState('');
  const [pluginDir, setPluginDir] = useState('');
  const [exePath, setExePath] = useState('');
  const [states, setStates] = useState<Record<string, ProviderState>>(
    Object.fromEntries(PROVIDERS.map((p) => [p.id, { status: 'loading' as Status }]))
  );
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [assetsInstalled, setAssetsInstalled] = useState<boolean | null>(null);

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev, line]);
  }, []);

  useEffect(() => {
    bridge.getPaths().then(async (p) => {
      setProjectDir(p.projectDir);
      setPluginDir(p.pluginDir);
      setExePath(p.exePath);
      await refreshStatuses(p.projectDir, p.exePath);
      checkAssetsInstalled(p.projectDir);
    });
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  async function refreshStatuses(dir: string, exe: string) {
    const newStates: Record<string, ProviderState> = {};
    for (const provider of PROVIDERS) {
      const configPath = provider.configPath(dir);
      const content = await bridge.readFile(configPath);
      let status: ProviderStatus;
      if (provider.format === 'toml') {
        status = getTomlProviderStatus(content);
      } else {
        status = getJsonProviderStatus(content, provider.configKey, exe);
      }
      newStates[provider.id] = { status };
    }
    setStates(newStates);
  }

  function checkAssetsInstalled(dir: string) {
    bridge.readFile(`${dir}/AGENTS.md`).then((content) => {
      setAssetsInstalled(content !== null);
    });
  }

  async function configureProvider(providerId: string) {
    setLogs([]);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    const scriptPath = `${pluginDir}\\Scripts\\Generate-ClientConfig.ps1`;
    await bridge.runScript(scriptPath, ['-Client', providerId]);
    unsub();
    setRunning(false);
    await refreshStatuses(projectDir, exePath);
  }

  async function configureAll() {
    setLogs([]);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    const scriptPath = `${pluginDir}\\Scripts\\Generate-ClientConfig.ps1`;
    await bridge.runScript(scriptPath, ['-Client', 'All']);
    unsub();
    setRunning(false);
    await refreshStatuses(projectDir, exePath);
  }

  async function deployAssets() {
    setLogs([]);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    const scriptPath = `${pluginDir}\\Scripts\\Install-ClaudeAssets.ps1`;
    await bridge.runScript(scriptPath, ['-ProjectRoot', projectDir]);
    unsub();
    setRunning(false);
    checkAssetsInstalled(projectDir);
  }

  function openConfigFile(path: string) {
    bridge.openExternal(`file:///${path.replace(/\\/g, '/')}`);
  }

  return (
    <div className="p-6 max-w-4xl">
      <div className="flex items-center justify-between mb-6">
        <div>
          <h1 className="text-xl font-semibold text-white mb-1">AI Providers</h1>
          <p className="text-gray-500 text-sm">Configure MCP entries for each AI client.</p>
        </div>
        <button
          onClick={configureAll}
          disabled={running}
          className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white"
        >
          Configure All
        </button>
      </div>

      <div className="grid grid-cols-1 gap-3 mb-8">
        {PROVIDERS.map((provider) => {
          const state = states[provider.id] ?? { status: 'loading' as Status };
          const configPath = projectDir ? provider.configPath(projectDir) : '';
          return (
            <div
              key={provider.id}
              className="bg-ue-panel border border-ue-border rounded p-4 flex items-center justify-between gap-4"
            >
              <div className="flex-1 min-w-0">
                <div className="flex items-center gap-3 mb-1">
                  <span className="text-sm font-medium text-white">{provider.label}</span>
                  <StatusBadge status={state.status} />
                </div>
                <div className="text-xs text-gray-500 truncate">{configPath}</div>
              </div>
              <div className="flex gap-2 flex-shrink-0">
                <button
                  onClick={() => openConfigFile(configPath)}
                  className="px-3 py-1.5 bg-ue-panel border border-ue-border rounded text-xs hover:bg-white/10"
                  title="Open config file"
                >
                  Open file
                </button>
                <button
                  onClick={() => configureProvider(provider.id)}
                  disabled={running}
                  className="px-3 py-1.5 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-xs text-white"
                >
                  Configure
                </button>
              </div>
            </div>
          );
        })}
      </div>

      {/* Skills & Agents */}
      <div className="border-t border-ue-border pt-6">
        <div className="flex items-center justify-between mb-4">
          <div>
            <h2 className="text-base font-medium text-white mb-1">Skills & Agents</h2>
            <p className="text-gray-500 text-xs">
              Claude Code skills, agents, and AGENTS.md for Claude/Copilot.
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

        {logs.length > 0 && (
          <div>
            <label className="block text-xs text-gray-400 mb-1">Output</label>
            <LogStream lines={logs} maxHeight="200px" />
          </div>
        )}
      </div>
    </div>
  );
}
