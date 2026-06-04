import { useState, useEffect, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import LogStream from '../components/LogStream';

export default function Install() {
  const [projectDir, setProjectDir] = useState('');
  const [engineDir, setEngineDir] = useState('');
  const [pluginDir, setPluginDir] = useState('');
  const [mountType, setMountType] = useState<'copy' | 'symlink'>('copy');
  const [buildServer, setBuildServer] = useState(true);
  const [applyPatches, setApplyPatches] = useState(false);
  const [client, setClient] = useState('All');
  const [logs, setLogs] = useState<string[]>([]);
  const [running, setRunning] = useState(false);
  const [exitCode, setExitCode] = useState<number | null>(null);

  useEffect(() => {
    bridge.getPaths().then((p) => {
      setProjectDir(p.projectDir);
      setEngineDir(p.engineDir);
      setPluginDir(p.pluginDir);
    });
  }, []);

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev, line]);
  }, []);

  async function browseProject() {
    const p = await bridge.openFileDialog({
      title: 'Select .uproject file',
      filters: [{ name: 'UE Project', extensions: ['uproject'] }],
      properties: ['openFile'],
    });
    if (p) setProjectDir(p.replace(/[/\\][^/\\]+\.uproject$/, ''));
  }

  async function browseEngine() {
    const p = await bridge.openFileDialog({
      title: 'Select Unreal Engine directory',
      properties: ['openDirectory'],
    });
    if (p) setEngineDir(p);
  }

  async function runInstall() {
    setLogs([]);
    setExitCode(null);
    setRunning(true);

    const unsub = bridge.onScriptLog(appendLog);

    const args: string[] = [
      '-Client', client,
      '-ProjectFile', `${projectDir}\\${projectDir.split(/[/\\]/).pop()}.uproject`,
      '-EngineDir', engineDir,
    ];
    if (mountType === 'symlink') args.push('-Symlink');
    if (applyPatches) args.push('-ApplyEnginePatches');
    if (!buildServer) args.push('-SkipBuild');

    const scriptPath = `${pluginDir}\\Scripts\\Install-Plugin.ps1`;
    const code = await bridge.runScript(scriptPath, args);

    unsub();
    setExitCode(code);
    setRunning(false);
  }

  return (
    <div className="p-6 max-w-3xl">
      <h1 className="text-xl font-semibold text-white mb-1">Install Plugin</h1>
      <p className="text-gray-500 text-sm mb-6">
        Mount BlueprintReader into a UE project, build the MCP server, and configure all AI clients.
      </p>

      <div className="space-y-4">
        {/* Project path */}
        <div>
          <label className="block text-xs text-gray-400 mb-1">Project directory</label>
          <div className="flex gap-2">
            <input
              className="flex-1 bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
              value={projectDir}
              onChange={(e) => setProjectDir(e.target.value)}
              placeholder="C:\Projects\MyGame"
            />
            <button
              onClick={browseProject}
              className="px-3 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10"
            >
              Browse
            </button>
          </div>
        </div>

        {/* Engine dir */}
        <div>
          <label className="block text-xs text-gray-400 mb-1">Engine directory</label>
          <div className="flex gap-2">
            <input
              className="flex-1 bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
              value={engineDir}
              onChange={(e) => setEngineDir(e.target.value)}
              placeholder="C:\Program Files\Epic Games\UE_5.8"
            />
            <button
              onClick={browseEngine}
              className="px-3 py-2 bg-ue-panel border border-ue-border rounded text-sm hover:bg-white/10"
            >
              Browse
            </button>
          </div>
        </div>

        {/* Mount type */}
        <div>
          <label className="block text-xs text-gray-400 mb-2">Mount type</label>
          <div className="flex gap-4">
            {(['copy', 'symlink'] as const).map((t) => (
              <label key={t} className="flex items-center gap-2 cursor-pointer">
                <input
                  type="radio"
                  name="mountType"
                  value={t}
                  checked={mountType === t}
                  onChange={() => setMountType(t)}
                  className="accent-ue-accent"
                />
                <span className="text-sm capitalize">{t}</span>
              </label>
            ))}
          </div>
        </div>

        {/* AI client */}
        <div>
          <label className="block text-xs text-gray-400 mb-1">Configure AI client(s)</label>
          <select
            value={client}
            onChange={(e) => setClient(e.target.value)}
            className="bg-black/40 border border-ue-border rounded px-3 py-2 text-sm text-gray-200 focus:outline-none focus:border-ue-accent"
          >
            {['All', 'ClaudeCode', 'Cursor', 'VSCode', 'Gemini', 'Codex'].map((c) => (
              <option key={c} value={c}>{c}</option>
            ))}
          </select>
        </div>

        {/* Options */}
        <div className="flex gap-6">
          <label className="flex items-center gap-2 cursor-pointer">
            <input
              type="checkbox"
              checked={buildServer}
              onChange={(e) => setBuildServer(e.target.checked)}
              className="accent-ue-accent"
            />
            <span className="text-sm">Build MCP server</span>
          </label>
          <label className="flex items-center gap-2 cursor-pointer">
            <input
              type="checkbox"
              checked={applyPatches}
              onChange={(e) => setApplyPatches(e.target.checked)}
              className="accent-ue-accent"
            />
            <span className="text-sm">Apply engine patches</span>
          </label>
        </div>

        {/* Run button */}
        <button
          onClick={runInstall}
          disabled={running || !projectDir}
          className="px-6 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 disabled:cursor-not-allowed rounded text-sm font-medium text-white transition-colors"
        >
          {running ? 'Installing…' : 'Install Plugin'}
        </button>

        {/* Log output */}
        {(logs.length > 0 || running) && (
          <div>
            <div className="flex items-center justify-between mb-1">
              <label className="text-xs text-gray-400">Output</label>
              {exitCode !== null && (
                <span className={`text-xs ${exitCode === 0 ? 'text-green-400' : 'text-red-400'}`}>
                  {exitCode === 0 ? '✓ Success' : `✗ Exit code ${exitCode}`}
                </span>
              )}
            </div>
            <LogStream lines={logs} maxHeight="320px" />
          </div>
        )}
      </div>
    </div>
  );
}
