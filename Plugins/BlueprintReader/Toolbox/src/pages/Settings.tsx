import { useState, useEffect } from 'react';

interface EnvFlag {
  key: string;
  label: string;
  default: string;
  type: 'bool' | 'string' | 'number' | 'select';
  options?: string[];
  description: string;
  group: string;
  docsAnchor?: string;
}

const FLAGS: EnvFlag[] = [
  // ── Backend ───────────────────────────────────────────────────────────────
  { key: 'BP_READER_BACKEND', label: 'Backend', default: 'auto', type: 'select',
    options: ['auto', 'mock', 'commandlet', 'live'],
    description: 'Which backend the server uses. auto=picks live when editor is open, commandlet otherwise. mock=no UE required (fixtures only).',
    group: 'Backend' },
  { key: 'BP_READER_PROJECT', label: 'Project (.uproject)', default: '(auto-discovered)', type: 'string',
    description: 'Path to your .uproject. Auto-discovered from the exe location when omitted.',
    group: 'Backend' },
  { key: 'BP_READER_ENGINE_DIR', label: 'Engine directory', default: '(auto-discovered)', type: 'string',
    description: 'Path to the UE engine root. Auto-discovered from .uproject EngineAssociation when omitted.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON', label: 'Daemon mode', default: '1', type: 'bool',
    description: 'Keep the editor commandlet alive between calls. Off=spawn a new process per call (slow but stateless).',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_IDLE_SECONDS', label: 'Daemon idle timeout (s)', default: '300', type: 'number',
    description: 'Daemon exits after this many seconds with no connected MCP client.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_MAX_LIFETIME_SECONDS', label: 'Daemon max lifetime (s)', default: '0 (off)', type: 'number',
    description: 'Hard cap on daemon lifetime. 0=disabled. Useful to force a periodic restart.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_WEDGE_SECONDS', label: 'Daemon wedge timeout (s)', default: '120', type: 'number',
    description: 'Off-game-thread watchdog: force-exits if the game thread stops responding for this many seconds. 0=disable.',
    group: 'Backend' },
  { key: 'BP_READER_TIMEOUT_SECONDS', label: 'Call timeout (s)', default: '120', type: 'number',
    description: 'Per-tool-call subprocess timeout.',
    group: 'Backend' },
  { key: 'BP_READER_PREWARM', label: 'Prewarm daemon on startup', default: '0', type: 'bool',
    description: 'Spawn the editor daemon in the background when the MCP server starts, hiding cold-start latency.',
    group: 'Backend' },
  { key: 'BP_READER_EDITOR_CONFIG', label: 'Editor config', default: 'Development', type: 'select',
    options: ['Development', 'DebugGame', 'Debug', 'Test', 'Shipping'],
    description: 'Which UnrealEditor-Cmd[-Config].exe the daemon launches. Must match the config your plugin DLL was built with.',
    group: 'Backend' },
  { key: 'BP_READER_EDITOR_ARGS', label: 'Extra editor args', default: '(empty)', type: 'string',
    description: 'Additional command-line args for UnrealEditor-Cmd.exe. Tip: -EnableAllPlugins prevents plugin startup failures.',
    group: 'Backend' },
  { key: 'BP_READER_PLUGIN_DENYLIST', label: 'Plugin denylist', default: '(empty)', type: 'string',
    description: 'Comma-separated plugin names to pass as -DisablePlugin=<name> when spawning the editor. Useful for plugins that crash on init (e.g. DLSS).',
    group: 'Backend' },

  // ── Permissions ───────────────────────────────────────────────────────────
  { key: 'BP_READER_ALLOW_WRITE', label: 'Enable write tools', default: '0', type: 'bool',
    description: 'Enables all mutating tools (add_variable, add_node, wire_pins, apply_ops, delete_*, etc.). Read-only by default to prevent accidental mutations.',
    group: 'Permissions', docsAnchor: '#read-only-mode' },
  { key: 'BP_READER_ALLOW_TRANSPILE', label: 'Enable transpile tools', default: '0', type: 'bool',
    description: 'Enables the 6 BP↔C++ transpile tools. Off by default — transpile writes source files and shells to UBT.',
    group: 'Permissions' },
  { key: 'BP_READER_ALLOW_PYTHON', label: 'Enable run_python_script', default: '0', type: 'bool',
    description: 'Enables run_python_script. Off by default — gives full unreal.* API access which bypasses all safety conventions.',
    group: 'Permissions' },
  { key: 'BP_READER_REQUIRE_CONFIRM', label: 'Require _confirm on destructive ops', default: '0', type: 'bool',
    description: 'When on, delete_asset / delete_variable / delete_node etc. require {"_confirm": true} in their args. Guards against accidental AI-agent deletions.',
    group: 'Permissions' },
  { key: 'BP_READER_AUTO_CHECKOUT', label: 'Auto source-control checkout', default: '1', type: 'bool',
    description: 'Before a write op, auto-checks assets out of Perforce/Git so the editor doesn\'t show a blocking modal.',
    group: 'Permissions' },

  // ── Tools surface ────────────────────────────────────────────────────────
  { key: 'BP_READER_TOOLS', label: 'Tool allow-list', default: '(all ~258)', type: 'string',
    description: 'Comma-separated tool names or category names to advertise. Use to fit under client tool-count caps (Copilot caps at 128 total). Example: core,cpp',
    group: 'Tools', docsAnchor: '#tool-filtering' },
  { key: 'BP_READER_TOOLS_EXCLUDE', label: 'Tool block-list', default: '(empty)', type: 'string',
    description: 'Comma-separated tool names or category names to hide. Applied after the allow-list. Example: materials,widgets,niagara',
    group: 'Tools' },
  { key: 'BP_READER_TOOL_ALLOW', label: 'Tool allow regex', default: '(empty)', type: 'string',
    description: 'ECMAScript regex patterns (comma-separated). Only tools matching at least one pattern are advertised. Example: ^(list|read|get|find)',
    group: 'Tools' },
  { key: 'BP_READER_TOOL_BLOCK', label: 'Tool block regex', default: '(empty)', type: 'string',
    description: 'ECMAScript regex patterns (comma-separated). Tools matching any pattern are hidden. Example: ^(build_lighting|cook_content)',
    group: 'Tools' },
  { key: 'BP_READER_PROGRESSIVE', label: 'Progressive disclosure', default: '0', type: 'bool',
    description: 'When on, starts with only the core ~35-tool surface and lets the agent widen via enable_tool_category. Saves context tokens on large projects.',
    group: 'Tools', docsAnchor: '#progressive-disclosure' },

  // ── Performance / caching ─────────────────────────────────────────────────
  { key: 'BP_READER_CACHE_TTL_SECONDS', label: 'Cache TTL (s)', default: '30', type: 'number',
    description: 'How long read-tool responses are memoized. 0=disable caching.',
    group: 'Performance' },
  { key: 'BP_READER_VERBOSE', label: 'Verbose (disable lean mode)', default: '0', type: 'bool',
    description: 'Disables lean-mode response stripping. By default, empty arrays and redundant link data are pruned to save tokens.',
    group: 'Performance' },

  // ── HTTP transport ────────────────────────────────────────────────────────
  { key: 'BP_READER_HTTP_PORT', label: 'HTTP port', default: '(stdio mode)', type: 'number',
    description: 'Set to a port (e.g. 7878) to run the server as an HTTP/SSE endpoint instead of stdio. Binds to 127.0.0.1 only.',
    group: 'HTTP transport' },
  { key: 'BP_READER_HTTP_PATH', label: 'HTTP path', default: '/mcp', type: 'string',
    description: 'URL path for the MCP endpoint when HTTP mode is enabled.',
    group: 'HTTP transport' },
];

const GROUPS = ['Backend', 'Permissions', 'Tools', 'Performance', 'HTTP transport'];

export default function Settings() {
  const [values, setValues] = useState<Record<string, string>>({});
  const [saved, setSaved] = useState(false);
  const [activeGroup, setActiveGroup] = useState('Backend');

  useEffect(() => {
    const stored = localStorage.getItem('bpr-env-overrides');
    if (stored) setValues(JSON.parse(stored));
  }, []);

  const set = (key: string, val: string) => {
    setValues(prev => ({ ...prev, [key]: val }));
    setSaved(false);
  };

  const save = () => {
    localStorage.setItem('bpr-env-overrides', JSON.stringify(values));
    setSaved(true);
    setTimeout(() => setSaved(false), 2000);
  };

  const copyEnvBlock = () => {
    const lines = Object.entries(values)
      .filter(([, v]) => v && v !== '(auto-discovered)' && v !== '(all ~258)' && v !== '(empty)' && v !== '(stdio mode)')
      .map(([k, v]) => `"${k}": "${v}"`);
    if (lines.length === 0) {
      navigator.clipboard.writeText('// No overrides set');
      return;
    }
    navigator.clipboard.writeText('{\n  "env": {\n    ' + lines.join(',\n    ') + '\n  }\n}');
  };

  const flags = FLAGS.filter(f => f.group === activeGroup);

  return (
    <div className="flex h-full">
      {/* Group nav */}
      <div className="w-40 flex-shrink-0 border-r border-ue-border p-2 space-y-1">
        {GROUPS.map(g => (
          <button key={g} onClick={() => setActiveGroup(g)}
            className={`w-full text-left px-3 py-2 rounded text-sm transition-colors ${
              activeGroup === g ? 'bg-ue-accent/20 text-ue-accent' : 'text-gray-400 hover:bg-white/5'
            }`}>
            {g}
          </button>
        ))}
      </div>

      {/* Flag editor */}
      <div className="flex-1 overflow-auto p-6">
        <div className="flex items-center justify-between mb-6">
          <h2 className="text-lg font-semibold text-white">{activeGroup}</h2>
          <div className="flex gap-2">
            <button onClick={copyEnvBlock}
              className="px-3 py-1.5 text-xs bg-ue-panel border border-ue-border rounded hover:bg-white/5 text-gray-300">
              Copy env block
            </button>
            <button onClick={save}
              className={`px-3 py-1.5 text-xs rounded font-medium transition-colors ${
                saved ? 'bg-green-600 text-white' : 'bg-ue-accent hover:bg-ue-accent/80 text-black'
              }`}>
              {saved ? '✓ Saved' : 'Save'}
            </button>
          </div>
        </div>

        <div className="space-y-4">
          {flags.map(flag => (
            <div key={flag.key} className="bg-ue-panel rounded-lg p-4 border border-ue-border">
              <div className="flex items-start justify-between gap-4">
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 mb-1">
                    <span className="text-sm font-medium text-white">{flag.label}</span>
                    <span className="text-xs text-gray-500 font-mono">{flag.key}</span>
                    <span className="text-xs text-gray-600">default: {flag.default}</span>
                  </div>
                  <p className="text-xs text-gray-400 leading-relaxed">{flag.description}</p>
                </div>
                <div className="flex-shrink-0 w-48">
                  {flag.type === 'bool' ? (
                    <select value={values[flag.key] ?? ''} onChange={e => set(flag.key, e.target.value)}
                      className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white focus:border-ue-accent outline-none">
                      <option value="">— use default —</option>
                      <option value="1">1 (enabled)</option>
                      <option value="0">0 (disabled)</option>
                    </select>
                  ) : flag.type === 'select' ? (
                    <select value={values[flag.key] ?? ''} onChange={e => set(flag.key, e.target.value)}
                      className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white focus:border-ue-accent outline-none">
                      <option value="">— use default —</option>
                      {flag.options!.map(o => <option key={o} value={o}>{o}</option>)}
                    </select>
                  ) : (
                    <input type={flag.type === 'number' ? 'number' : 'text'}
                      placeholder={flag.default}
                      value={values[flag.key] ?? ''}
                      onChange={e => set(flag.key, e.target.value)}
                      className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white placeholder-gray-600 focus:border-ue-accent outline-none" />
                  )}
                </div>
              </div>
            </div>
          ))}
        </div>

        <p className="mt-6 text-xs text-gray-600">
          These settings are stored locally and used by the Tester page when starting the server.
          Click "Copy env block" to paste the JSON snippet into your .mcp.json or MCP client config.
        </p>
      </div>
    </div>
  );
}
