import { useState, useEffect, useCallback, useRef } from 'react';
import { bridge } from '../lib/bridge';
import LogStream from '../components/LogStream';
import StatusBadge from '../components/StatusBadge';
import { loadUproject, uprojectToPluginDir } from '../lib/paths';
import { cmpVersion as cmp } from '../lib/semver';

const REPO_RELEASES = 'https://github.com/defessler/Unreal-Engine-5-MCP/releases';

// Semver-aware (TBX-F8) — tolerates null/undefined like the old local copy.
function cmpVersion(a?: string | null, b?: string | null): number {
  return cmp(a ?? '', b ?? '');
}

export default function Update() {
  const [uproject, setUproject] = useState(() => loadUproject());
  const [toolboxVersion, setToolboxVersion] = useState('');
  const [pluginVersion, setPluginVersion] = useState<string | null>(null);
  const [latestTag, setLatestTag] = useState<string | null>(null);
  const [checking, setChecking] = useState(false);
  const [refreshError, setRefreshError] = useState('');
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);

  // TBX-R9: mounted guard + unsub-on-unmount for the script-log listener.
  const mountedRef = useRef(true);
  const unsubRef = useRef<(() => void) | null>(null);
  useEffect(() => () => { mountedRef.current = false; unsubRef.current?.(); unsubRef.current = null; }, []);

  const appendLog = useCallback((line: string) => { if (mountedRef.current) setLogs((p) => [...p, line]); }, []);

  const pluginDir = uprojectToPluginDir(uproject);

  // Installed plugin version from the mounted .uplugin VersionName.
  const readPluginVersion = useCallback(async (dir: string) => {
    if (!dir) { setPluginVersion(null); return; }
    const up = await bridge.readFile(`${dir}\\BlueprintReader.uplugin`);
    if (up) { try { setPluginVersion(JSON.parse(up).VersionName ?? null); } catch { setPluginVersion(null); } }
    else setPluginVersion(null);
  }, []);

  const refresh = useCallback(async () => {
    setChecking(true);
    setToolboxVersion(await bridge.getAppVersion());
    // TBX-P5: a failed release check used to silently null the tag and disable
    // both buttons with no reason shown. Surface the network/rate-limit error.
    try {
      const rel = await bridge.getLatestRelease();
      if (rel.ok && rel.tag) { setLatestTag(rel.tag); setRefreshError(''); }
      else { setRefreshError((rel as { error?: string }).error ?? 'Could not reach GitHub (offline or rate-limited).'); }
    } catch (e) {
      setRefreshError(e instanceof Error ? e.message : 'Could not reach GitHub.');
    }
    setChecking(false);
  }, []);

  useEffect(() => {
    bridge.getPaths().then((p) => {
      const saved = loadUproject() || p.uproject;
      if (saved && saved !== uproject) setUproject(saved);
    });
    refresh();
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  // Re-read the plugin version whenever the resolved project changes — the
  // mount-time getPaths() resolves uproject asynchronously, after refresh ran
  // with an empty pluginDir, so this catches the real installed version.
  useEffect(() => { readPluginVersion(pluginDir); }, [pluginDir, readPluginVersion]);

  async function updateToolbox() {
    setLogs([]); setRunning(true);
    unsubRef.current = bridge.onScriptLog(appendLog);
    const res = await bridge.selfUpdateToolbox();
    unsubRef.current?.(); unsubRef.current = null;
    if (!res.ok) { appendLog(`[error] ${res.error ?? 'self-update failed'}`); setRunning(false); }
    else if (res.upToDate) { appendLog('Toolbox is already up to date.'); setRunning(false); }
    else { appendLog('Toolbox update downloaded — restarting to apply…'); }
    // On success (!upToDate) keep running=true: the app quits + relaunches in a
    // moment; leaving the button disabled stops a second click from spawning a
    // competing swap helper during the ~900ms quit window.
  }

  async function updatePlugin() {
    if (!uproject) { appendLog('[error] No project configured — set one on the Install tab first.'); return; }
    setLogs([]); setRunning(true);
    unsubRef.current = bridge.onScriptLog(appendLog);
    const res = await bridge.installPluginFromRelease({ uproject, client: 'All' });
    unsubRef.current?.(); unsubRef.current = null;
    if (!res.ok && res.error) appendLog(`[error] ${res.error}`);
    setRunning(false);
    await refresh();
    await readPluginVersion(pluginDir);
  }

  const toolboxOutdated = !!latestTag && cmpVersion(latestTag, toolboxVersion) > 0;
  const pluginOutdated = !!latestTag && !!pluginVersion && cmpVersion(latestTag, pluginVersion) > 0;

  return (
    <div className="p-6 max-w-2xl">
      <div className="flex items-center justify-between mb-1">
        <h1 className="text-xl font-semibold text-white">Update</h1>
        <button
          onClick={refresh}
          disabled={checking || running}
          className="px-3 py-1.5 bg-ue-panel border border-ue-border rounded text-xs hover:bg-white/10 disabled:opacity-50 text-gray-300"
        >
          {checking ? 'Checking…' : 'Check for updates'}
        </button>
      </div>
      <p className="text-gray-500 text-sm mb-6">Keep the Toolbox and the plugin up to date — both pull from the latest GitHub release.</p>

      {refreshError && (
        <div className="mb-4 text-xs text-amber-400 bg-amber-400/5 border border-amber-400/20 rounded px-3 py-2" role="alert">
          Couldn’t check for updates: {refreshError}
        </div>
      )}

      {/* Toolbox self-update */}
      <div className="bg-ue-panel border border-ue-border rounded p-5 mb-4">
        <div className="flex items-center justify-between gap-4">
          <div>
            <div className="text-sm font-medium text-white mb-1">Toolbox app</div>
            <div className="text-xs text-gray-500">
              Installed <span className="text-gray-300">v{toolboxVersion || '—'}</span>
              {latestTag && <> · Latest <span className="text-gray-300">{latestTag}</span></>}
            </div>
          </div>
          <div className="flex items-center gap-3 flex-shrink-0">
            {latestTag && (
              <StatusBadge status={toolboxOutdated ? 'stale' : 'configured'} label={toolboxOutdated ? 'Update available' : 'Up to date'} />
            )}
            <button
              onClick={updateToolbox}
              disabled={running || !toolboxOutdated}
              className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-40 rounded text-sm font-medium text-white"
            >
              Update Toolbox
            </button>
          </div>
        </div>
        <div className="text-xs text-gray-600 mt-2">Downloads the new portable exe, then restarts to apply.</div>
      </div>

      {/* Plugin update */}
      <div className="bg-ue-panel border border-ue-border rounded p-5 mb-6">
        <div className="flex items-center justify-between gap-4">
          <div>
            <div className="text-sm font-medium text-white mb-1">BlueprintReader plugin</div>
            <div className="text-xs text-gray-500">
              {uproject
                ? <>Installed <span className="text-gray-300">{pluginVersion ? `v${pluginVersion}` : 'not installed'}</span>{latestTag && <> · Latest <span className="text-gray-300">{latestTag}</span></>}</>
                : 'No project configured — set one on the Install tab.'}
            </div>
          </div>
          <div className="flex items-center gap-3 flex-shrink-0">
            {latestTag && pluginVersion && (
              <StatusBadge status={pluginOutdated ? 'stale' : 'configured'} label={pluginOutdated ? 'Update available' : 'Up to date'} />
            )}
            <button
              onClick={updatePlugin}
              disabled={running || !uproject || !latestTag || (!!pluginVersion && !pluginOutdated)}
              className="px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-40 rounded text-sm font-medium text-white"
            >
              {pluginVersion ? 'Update plugin' : 'Install plugin'}
            </button>
          </div>
        </div>
        <div className="text-xs text-gray-600 mt-2">Re-downloads the latest release and re-mounts it — your local build is preserved; client configs are refreshed for all supported clients.</div>
      </div>

      {logs.length > 0 && (
        <div className="mb-4">
          <label className="block text-xs text-gray-500 mb-1">Output</label>
          <LogStream lines={logs} maxHeight="240px" />
        </div>
      )}

      <button
        onClick={() => bridge.openExternal(REPO_RELEASES)}
        className="text-xs text-gray-500 hover:text-gray-300"
      >
        View all releases ↗
      </button>
    </div>
  );
}
