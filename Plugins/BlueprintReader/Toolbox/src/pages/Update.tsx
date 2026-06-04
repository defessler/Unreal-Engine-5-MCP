import { useState, useEffect, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import LogStream from '../components/LogStream';
import StatusBadge from '../components/StatusBadge';

interface UpdateCache {
  checked_iso?: string;
  latest_tag?: string;
  current?: string;
  update_available?: boolean;
}

export default function Update() {
  const [projectDir, setProjectDir] = useState('');
  const [pluginDir, setPluginDir] = useState('');
  const [cache, setCache] = useState<UpdateCache | null>(null);
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [checkedAt, setCheckedAt] = useState('');

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev, line]);
  }, []);

  useEffect(() => {
    bridge.getPaths().then(async (p) => {
      setProjectDir(p.projectDir);
      setPluginDir(p.pluginDir);
      const cachePath = `${p.projectDir}\\Saved\\bp-reader-update.json`;
      const content = await bridge.readFile(cachePath);
      if (content) {
        try {
          const c = JSON.parse(content) as UpdateCache;
          setCache(c);
          if (c.checked_iso) {
            setCheckedAt(new Date(c.checked_iso).toLocaleString());
          }
        } catch {
          // ignore malformed cache
        }
      }
    });
  }, []);

  async function checkNow() {
    setLogs([]);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    const scriptPath = `${pluginDir}\\Scripts\\Check-Update.ps1`;
    await bridge.runScript(scriptPath, []);
    unsub();
    setRunning(false);

    // Reload cache
    const cachePath = `${projectDir}\\Saved\\bp-reader-update.json`;
    const content = await bridge.readFile(cachePath);
    if (content) {
      try {
        const c = JSON.parse(content) as UpdateCache;
        setCache(c);
        if (c.checked_iso) setCheckedAt(new Date(c.checked_iso).toLocaleString());
      } catch {
        // ignore
      }
    }
  }

  async function runUpdate() {
    setLogs([]);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    const scriptPath = `${pluginDir}\\Scripts\\Update-Plugin.ps1`;
    await bridge.runScript(scriptPath, []);
    unsub();
    setRunning(false);
    await checkNow();
  }

  const repoUrl = 'https://github.com/defessler/Unreal-Engine-5-MCP/releases';

  return (
    <div className="p-6 max-w-2xl">
      <h1 className="text-xl font-semibold text-white mb-1">Update</h1>
      <p className="text-gray-500 text-sm mb-6">Check for and install plugin updates.</p>

      <div className="bg-ue-panel border border-ue-border rounded p-5 mb-6">
        <div className="grid grid-cols-2 gap-4 mb-4">
          <div>
            <div className="text-xs text-gray-500 mb-1">Current version</div>
            <div className="text-sm text-white font-medium">{cache?.current ?? '—'}</div>
          </div>
          <div>
            <div className="text-xs text-gray-500 mb-1">Latest release</div>
            <div className="flex items-center gap-2">
              <div className="text-sm text-white font-medium">{cache?.latest_tag ?? '—'}</div>
              {cache?.update_available !== undefined && (
                <StatusBadge
                  status={cache.update_available ? 'stale' : 'configured'}
                  label={cache.update_available ? 'Update available' : 'Up to date'}
                />
              )}
            </div>
          </div>
        </div>

        {checkedAt && (
          <div className="text-xs text-gray-600">Last checked: {checkedAt}</div>
        )}

        <div className="flex gap-3 mt-4">
          <button
            onClick={checkNow}
            disabled={running}
            className="px-4 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10 disabled:opacity-50"
          >
            {running ? 'Checking…' : 'Check Now'}
          </button>
          {cache?.update_available && (
            <button
              onClick={runUpdate}
              disabled={running}
              className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white"
            >
              Update
            </button>
          )}
          <button
            onClick={() => bridge.openExternal(repoUrl)}
            className="px-4 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10 ml-auto"
          >
            View releases ↗
          </button>
        </div>
      </div>

      {logs.length > 0 && (
        <div>
          <label className="block text-xs text-gray-400 mb-1">Output</label>
          <LogStream lines={logs} maxHeight="240px" />
        </div>
      )}

      <div className="mt-6 p-4 bg-yellow-400/5 border border-yellow-400/20 rounded text-xs text-yellow-400/80">
        Note: Update-Plugin.ps1 downloads the latest source but does <strong>not</strong> rebuild the MCP server exe.
        After updating, rebuild from the Install page if server sources changed.
      </div>
    </div>
  );
}
