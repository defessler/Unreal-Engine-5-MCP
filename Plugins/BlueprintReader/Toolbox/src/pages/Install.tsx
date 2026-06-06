import { useState, useEffect, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import LogStream from '../components/LogStream';
import { storeUproject, loadUproject, uprojectToPluginDir } from '../lib/paths';

export default function Install() {
  // Seed from localStorage first so we don't flash empty UI even on first load.
  const [uproject, setUproject] = useState(() => loadUproject());
  const [engineDir, setEngineDir] = useState('');
  const [engineStatus, setEngineStatus] = useState<'idle' | 'resolving' | 'found' | 'missing'>('idle');
  const [mountType, setMountType] = useState<'copy' | 'symlink'>('copy');
  // Default OFF: the bundle ships a precompiled, engine-independent MCP server,
  // so no build is needed. Ticking this rebuilds it from source (optional).
  const [buildServer, setBuildServer] = useState(false);
  const [applyPatches, setApplyPatches] = useState(false);
  const [client, setClient] = useState('All');
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [exitCode, setExitCode] = useState<number | null>(null);

  // pluginDir is always derived from the current uproject — never from getPaths()
  // so it's always correct even on first launch before userData is written.
  const pluginDir = uprojectToPluginDir(uproject);

  useEffect(() => {
    bridge.getPaths().then(async (p) => {
      // Use saved uproject from userData if localStorage doesn't have one yet.
      const saved = loadUproject() || p.uproject;
      if (saved && saved !== uproject) {
        setUproject(saved);
        storeUproject(saved);
        await resolveEngine(saved);
      } else if (uproject) {
        await resolveEngine(uproject);
      }
    });
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev, line]);
  }, []);

  async function resolveEngine(uprojectPath: string) {
    if (!uprojectPath) { setEngineDir(''); setEngineStatus('idle'); return; }
    // Persist to both localStorage (same-session cross-page) and userData (next launch).
    storeUproject(uprojectPath);
    bridge.saveProject(uprojectPath);
    setEngineStatus('resolving');
    const dir = await bridge.resolveEngine(uprojectPath);
    if (dir) {
      setEngineDir(dir);
      setEngineStatus('found');
    } else {
      setEngineDir('');
      setEngineStatus('missing');
    }
  }

  async function browseUproject() {
    const p = await bridge.openFileDialog({
      title: 'Select your .uproject file',
      filters: [{ name: 'Unreal Project', extensions: ['uproject'] }],
      properties: ['openFile'],
    });
    if (p) {
      setUproject(p);
      await resolveEngine(p);
    }
  }

  async function runInstall() {
    setLogs([]);
    setExitCode(null);
    setRunning(true);

    const unsub = bridge.onScriptLog(appendLog);

    const scriptPath = `${pluginDir}\\Scripts\\Install-Plugin.ps1`;
    // The Toolbox configures an already-mounted plugin; it doesn't carry the
    // plugin tree itself. If the script isn't there, the plugin folder hasn't
    // been unzipped into <Project>/Plugins/ yet — say so instead of a bare
    // non-zero exit code.
    const present = await bridge.readFile(scriptPath);
    if (present === null) {
      appendLog(`[error] Install-Plugin.ps1 not found at ${scriptPath}`);
      appendLog(`Unzip the BlueprintReader plugin into ${pluginDir} (its parent Plugins/ folder) first, then run Install again.`);
      unsub();
      setExitCode(1);
      setRunning(false);
      return;
    }

    const args: string[] = ['-Client', client, '-ProjectFile', uproject];
    if (engineDir) args.push('-EngineDir', engineDir);
    if (mountType === 'symlink') args.push('-Symlink');
    // Engine patches only apply to a from-source build; guard on buildServer so
    // a stale applyPatches flag can't mutate the engine when the user opted out.
    if (buildServer && applyPatches) args.push('-ApplyEnginePatches');
    if (!buildServer) args.push('-SkipBuild');

    const code = await bridge.runScript(scriptPath, args);

    unsub();
    setExitCode(code);
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
        Mount BlueprintReader into your project and configure AI clients. The MCP server ships precompiled.
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
        </div>

        {/* Engine directory — shown when detected (with override option) or when detection failed */}
        {(engineStatus === 'found' || engineStatus === 'missing') && (
          <div>
            <label className="block text-xs font-medium text-gray-300 mb-1">
              Engine directory
              {engineStatus === 'found' && <span className="text-gray-500 font-normal ml-1">(auto-detected — override if needed)</span>}
            </label>
            <div className="flex gap-2">
              <input
                className="flex-1 bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
                value={engineDir}
                onChange={(e) => setEngineDir(e.target.value)}
                placeholder="C:\Program Files\Epic Games\UE_5.8"
              />
              <button
                onClick={async () => {
                  const p = await bridge.openFileDialog({
                    title: 'Select Unreal Engine directory',
                    properties: ['openDirectory'],
                  });
                  if (p) setEngineDir(p);
                }}
                className="px-3 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10 text-gray-300"
              >
                Browse
              </button>
            </div>
          </div>
        )}

        {/* Mount type */}
        <div>
          <label className="block text-xs font-medium text-gray-300 mb-2">Mount type</label>
          <div className="flex flex-col gap-2.5">
            <label className="flex items-start gap-3 cursor-pointer">
              <input type="radio" name="mountType" value="copy" checked={mountType === 'copy'} onChange={() => setMountType('copy')} className="mt-0.5 accent-ue-accent" />
              <div>
                <div className="text-sm text-gray-200">Copy</div>
                <div className="text-xs text-gray-500">Plugin files are copied into your project's Plugins/ folder. No admin rights needed.</div>
              </div>
            </label>
            <label className="flex items-start gap-3 cursor-pointer">
              <input type="radio" name="mountType" value="symlink" checked={mountType === 'symlink'} onChange={() => setMountType('symlink')} className="mt-0.5 accent-ue-accent" />
              <div>
                <div className="text-sm text-gray-200">Symlink</div>
                <div className="text-xs text-gray-500">Directory junction from your project to the unzipped plugin folder elsewhere on disk. Updates to that folder take effect immediately without re-copying.</div>
              </div>
            </label>
          </div>
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
            ✓ The release plugin ZIP ships a <strong>precompiled</strong>, engine-independent MCP server — by default no compilation is needed. Make sure the BlueprintReader plugin folder is already in <code>&lt;Project&gt;/Plugins/</code> before installing (the Toolbox configures the mounted plugin; it doesn't download it). The editor plugin module compiles automatically the first time you open your project in Unreal.
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
                Off by default — the bundled <code className="text-gray-400">BlueprintReaderMcp.exe</code> is used as-is. Tick only to rebuild from source (e.g. after editing server code). Auto-picks CMake + MSVC on an installed/Launcher engine or UBT on a source engine — no separate prebuild step. ~2 min cold, ~10 s incremental.
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
          disabled={running || !uproject}
          className="px-6 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-40 disabled:cursor-not-allowed rounded text-sm font-semibold text-white transition-colors"
        >
          {running ? 'Installing…' : 'Install Plugin'}
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
