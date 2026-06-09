import { useState, useEffect, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import LogStream from '../components/LogStream';
import { storeUproject, loadUproject } from '../lib/paths';

export default function Install() {
  const [uproject, setUproject] = useState(() => loadUproject());
  const [engineDir, setEngineDir] = useState('');
  const [engineStatus, setEngineStatus] = useState<'idle' | 'resolving' | 'found' | 'missing'>('idle');
  // Default OFF: the downloaded release ships a precompiled, engine-independent
  // MCP server, so no build is needed. Ticking this rebuilds it from source.
  const [buildServer, setBuildServer] = useState(false);
  const [applyPatches, setApplyPatches] = useState(false);
  const [client, setClient] = useState('All');
  const [latestTag, setLatestTag] = useState<string | null>(null);
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [exitCode, setExitCode] = useState<number | null>(null);
  // TBX-F5: validate the .uproject (extension + on-disk existence) so install
  // can't run against a path that can't work.
  const [uprojectExists, setUprojectExists] = useState<boolean | null>(null);
  const uprojectValid = uproject.toLowerCase().endsWith('.uproject') && uprojectExists === true;

  useEffect(() => {
    bridge.getPaths().then(async (p) => {
      const saved = loadUproject() || p.uproject;
      if (saved && saved !== uproject) {
        setUproject(saved);
        storeUproject(saved);
        await resolveEngine(saved);
      } else if (uproject) {
        await resolveEngine(uproject);
      }
    });
    bridge.getLatestRelease().then((r) => { if (r.ok && r.tag) setLatestTag(r.tag); }).catch(() => {});
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev, line]);
  }, []);

  async function resolveEngine(uprojectPath: string) {
    if (!uprojectPath) { setEngineDir(''); setEngineStatus('idle'); setUprojectExists(null); return; }
    // F5: confirm the file is a .uproject that exists. Uses a dedicated boolean
    // IPC (not the allowlist-gated read-file), so a first pick on a drive the
    // allowlist doesn't yet include (project root not persisted) still validates.
    setUprojectExists(await bridge.uprojectExists(uprojectPath));
    storeUproject(uprojectPath);
    bridge.saveProject(uprojectPath);
    setEngineStatus('resolving');
    const dir = await bridge.resolveEngine(uprojectPath);
    if (dir) { setEngineDir(dir); setEngineStatus('found'); }
    else { setEngineDir(''); setEngineStatus('missing'); }
  }

  async function browseUproject() {
    const p = await bridge.openFileDialog({
      title: 'Select your .uproject file',
      filters: [{ name: 'Unreal Project', extensions: ['uproject'] }],
      properties: ['openFile'],
    });
    if (p) { setUproject(p); await resolveEngine(p); }
  }

  async function runInstall() {
    setLogs([]);
    setExitCode(null);
    setRunning(true);
    const unsub = bridge.onScriptLog(appendLog);
    // The Toolbox downloads the latest plugin ZIP from GitHub (carrying the
    // precompiled server exe) and mounts it into the project — no pre-existing
    // in-project plugin required.
    const res = await bridge.installPluginFromRelease({
      uproject,
      client,
      build: buildServer,
      engineDir: buildServer ? engineDir : undefined,
      applyPatches: buildServer && applyPatches,
    });
    unsub();
    if (!res.ok && res.error) appendLog(`[error] ${res.error}`);
    setExitCode(res.ok ? 0 : (res.code ?? 1));
    setRunning(false);
  }

  const engineLabel = () => {
    if (engineStatus === 'resolving') return <span className="text-xs text-yellow-400">Detecting engine…</span>;
    if (engineStatus === 'found') return <span className="text-xs text-green-400">✓ {engineDir}</span>;
    if (engineStatus === 'missing') return <span className="text-xs text-amber-400">Engine not detected — select it below</span>;
    return null;
  };

  return (
    <div className="p-6 max-w-2xl">
      <h1 className="text-xl font-semibold text-white mb-1">Install Plugin</h1>
      <p className="text-gray-500 text-sm mb-6">
        Download the latest BlueprintReader release, install it into your project, and configure AI clients — all in one click.
      </p>

      <div className="space-y-5">
        {/* uproject */}
        <div>
          <label className="block text-xs font-medium text-gray-300 mb-1">Project file</label>
          <div className="flex gap-2">
            <input
              className="flex-1 bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
              value={uproject}
              onChange={async (e) => { setUproject(e.target.value); await resolveEngine(e.target.value); }}
              placeholder="C:\Projects\MyGame\MyGame.uproject"
            />
            <button
              onClick={browseUproject}
              className="px-3 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10 text-gray-300"
            >
              Browse
            </button>
          </div>
          <div className="mt-1.5 min-h-[18px] text-xs">{engineLabel()}</div>
          {uproject && !uprojectValid && (
            <div className="mt-1 text-xs text-red-400">
              {!uproject.toLowerCase().endsWith('.uproject')
                ? 'Path must end in .uproject'
                : uprojectExists === false ? 'File not found at that path' : 'Checking…'}
            </div>
          )}
        </div>

        {/* AI clients */}
        <div>
          <label className="block text-xs font-medium text-gray-300 mb-1">Configure AI client(s)</label>
          <select
            value={client}
            onChange={(e) => setClient(e.target.value)}
            className="bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
          >
            <option value="All">All supported clients</option>
            <option value="ClaudeCode">Claude Code</option>
            <option value="Cursor">Cursor</option>
            <option value="VSCode">VS Code / Copilot</option>
            <option value="Rider">JetBrains Rider</option>
            <option value="Gemini">Gemini CLI</option>
            <option value="Codex">OpenAI Codex</option>
          </select>
        </div>

        {/* Options */}
        <div className="space-y-4">
          <div className="text-xs text-green-400/90 bg-green-400/5 border border-green-400/20 rounded px-3 py-2">
            ✓ The Toolbox downloads the latest release {latestTag ? <strong>({latestTag})</strong> : ''} and installs it for you — the bundle includes a <strong>precompiled</strong>, engine-independent MCP server, so no compilation is needed. The editor plugin module compiles automatically the first time you open your project in Unreal.
          </div>
          <label className="flex items-start gap-3 cursor-pointer">
            <input
              type="checkbox"
              checked={buildServer}
              onChange={(e) => { const v = e.target.checked; setBuildServer(v); if (!v) setApplyPatches(false); }}
              className="mt-0.5 accent-ue-accent"
            />
            <div>
              <div className="text-sm text-gray-200">Rebuild MCP server from source <span className="text-gray-500 font-normal">(optional)</span></div>
              <div className="text-xs text-gray-500 mt-0.5">
                Off by default — the downloaded <code className="text-gray-400">BlueprintReaderMcp.exe</code> is used as-is. Tick only to rebuild from source after install. Auto-picks CMake + MSVC on an installed/Launcher engine or UBT on a source engine. ~2 min cold.
              </div>
            </div>
          </label>
          {buildServer && (
            <label className="flex items-start gap-3 cursor-pointer pl-7">
              <input
                type="checkbox"
                checked={applyPatches}
                onChange={(e) => setApplyPatches(e.target.checked)}
                className="mt-0.5 accent-ue-accent"
              />
              <div>
                <div className="text-sm text-gray-200">Apply engine patches</div>
                <div className="text-xs text-gray-500 mt-0.5">
                  Patches three <code className="text-gray-400">.Build.cs</code> files in the engine's source tree to fix include-path resolution. Only needed when building against a <strong className="text-gray-300">source-built engine</strong> — skip for Epic Games Launcher installs. Safe to re-run.
                </div>
              </div>
            </label>
          )}
        </div>

        {/* Run */}
        <button
          onClick={runInstall}
          disabled={running || !uprojectValid}
          className="px-6 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-40 disabled:cursor-not-allowed rounded text-sm font-semibold text-white transition-colors"
        >
          {running ? 'Installing…' : 'Download & Install Plugin'}
        </button>

        {/* Log */}
        {(logs.length > 0 || running) && (
          <div>
            <div className="flex items-center justify-between mb-1">
              <label className="text-xs text-gray-500">Output</label>
              {exitCode !== null && (
                <span className={`text-xs ${exitCode === 0 ? 'text-green-400' : 'text-red-400'}`}>
                  {exitCode === 0 ? '✓ Success' : `✗ Exit code ${exitCode}`}
                </span>
              )}
            </div>
            <LogStream lines={logs} maxHeight="280px" />
          </div>
        )}
      </div>
    </div>
  );
}
