import { useState, useEffect } from 'react';

interface EnvFlag {
  key: string;
  label: string;
  default: string;
  type: 'bool' | 'string' | 'number' | 'select' | 'editorargs';
  options?: string[];
  description: string;
  group: string;
}

// Known editor arg flags and whether they're mutually exclusive with others
const EDITOR_ARGS: { flag: string; label: string; desc: string; mutex?: string[] }[] = [
  { flag: '-EnableAllPlugins',        label: 'Enable all plugins',           desc: 'Prevents plugin startup failures when some plugins are missing or incompatible.' },
  { flag: '-SkipBuildToolVersionChecks', label: 'Skip build tool version checks', desc: 'Skips UBT version mismatch warnings on startup. Useful with older toolchains.' },
  { flag: '-nosplash',                label: 'No splash screen',             desc: 'Suppresses the UE splash screen on commandlet startup.' },
  { flag: '-nullrhi',                 label: 'Null RHI (no GPU)',             desc: 'Runs without a graphics device. Required for headless/server environments. Cannot be used alongside -game.', mutex: ['-game'] },
  { flag: '-game',                    label: 'Game mode',                    desc: 'Runs in game mode instead of editor mode. Cannot be combined with -nullrhi.', mutex: ['-nullrhi'] },
];

const FLAGS: EnvFlag[] = [
  // ── Backend ───────────────────────────────────────────────────────────────
  { key: 'BP_READER_BACKEND', label: 'Backend', default: 'auto', type: 'select',
    options: ['auto', 'mock', 'commandlet', 'live'],
    description: 'auto picks live when the editor is open, commandlet otherwise. mock uses fixture files only (no UE).',
    group: 'Backend' },
  { key: 'BP_READER_PROJECT', label: 'Project (.uproject)', default: '(auto-discovered)', type: 'string',
    description: 'Path to your .uproject. Auto-discovered from the exe location when omitted.',
    group: 'Backend' },
  { key: 'BP_READER_ENGINE_DIR', label: 'Engine directory', default: '(auto-discovered)', type: 'string',
    description: 'Path to the UE engine root. Auto-discovered from .uproject EngineAssociation when omitted.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON', label: 'Daemon mode', default: '1', type: 'bool',
    description: 'Keep the editor commandlet alive between calls. Disable to spawn a new process per call (slow but stateless).',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_IDLE_SECONDS', label: 'Daemon idle timeout (s)', default: '300', type: 'number',
    description: 'Daemon exits this many seconds after the last MCP client disconnects.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_MAX_LIFETIME_SECONDS', label: 'Daemon max lifetime (s)', default: '3600', type: 'number',
    description: 'Hard cap on daemon wall-clock lifetime regardless of activity. Set to 0 to disable.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_GRACE_SECONDS', label: 'Daemon startup grace (s)', default: '120', type: 'number',
    description: 'If no client connects within this window after daemon start, the daemon exits (orphan cleanup). Set to 0 to disable.',
    group: 'Backend' },
  { key: 'BP_READER_DAEMON_WEDGE_SECONDS', label: 'Daemon wedge timeout (s)', default: '120', type: 'number',
    description: 'Watchdog exits the daemon if the game thread stops responding for this many seconds. 0 disables.',
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
  { key: 'BP_READER_EDITOR_ARGS', label: 'Extra editor args', default: '', type: 'editorargs',
    description: 'Additional command-line arguments for UnrealEditor-Cmd.exe.',
    group: 'Backend' },
  { key: 'BP_READER_PLUGIN_DENYLIST', label: 'Plugin denylist', default: '', type: 'string',
    description: 'Comma-separated plugin names to disable on commandlet launch. Use for plugins that crash on init (e.g. DLSS).',
    group: 'Backend' },

  // ── Permissions ───────────────────────────────────────────────────────────
  { key: 'BP_READER_ALLOW_WRITE', label: 'Enable write tools', default: '0', type: 'bool',
    description: 'Enables all mutating tools (add_variable, add_node, wire_pins, apply_ops, delete_*, etc.). Read-only by default.',
    group: 'Permissions' },
  { key: 'BP_READER_ALLOW_TRANSPILE', label: 'Enable transpile tools', default: '0', type: 'bool',
    description: 'Enables the 6 BP↔C++ transpile tools. Off by default — transpile writes source files and shells to UBT.',
    group: 'Permissions' },
  { key: 'BP_READER_ALLOW_PYTHON', label: 'Enable run_python_script', default: '0', type: 'bool',
    description: 'Enables run_python_script. Off by default — gives full unreal.* API access which bypasses all safety conventions.',
    group: 'Permissions' },
  { key: 'BP_READER_REQUIRE_CONFIRM', label: 'Require _confirm on destructive ops', default: '0', type: 'bool',
    description: 'When on, delete_asset / delete_variable / delete_node etc. require {"_confirm": true} in their args.',
    group: 'Permissions' },
  { key: 'BP_READER_AUTO_CHECKOUT', label: 'Auto source-control checkout', default: '1', type: 'bool',
    description: 'Before a write op, auto-checks assets out of Perforce/Git so the editor does not show a blocking modal.',
    group: 'Permissions' },

  // ── Tools surface ────────────────────────────────────────────────────────
  { key: 'BP_READER_TOOLS', label: 'Tool allow-list', default: '', type: 'string',
    description: 'Comma-separated tool names or category names to advertise. Useful for clients with tool-count caps (Copilot: 128 total). Example: core,cpp',
    group: 'Tools' },
  { key: 'BP_READER_TOOLS_EXCLUDE', label: 'Tool block-list', default: '', type: 'string',
    description: 'Comma-separated tool names or category names to hide. Applied after the allow-list. Example: materials,widgets,niagara',
    group: 'Tools' },
  { key: 'BP_READER_TOOL_ALLOW', label: 'Tool allow regex', default: '', type: 'string',
    description: 'Regex patterns (comma-separated). Only tools matching at least one are advertised. Example: ^(list|read|get|find)',
    group: 'Tools' },
  { key: 'BP_READER_TOOL_BLOCK', label: 'Tool block regex', default: '', type: 'string',
    description: 'Regex patterns (comma-separated). Tools matching any pattern are hidden. Example: ^(build_lighting|cook_content)',
    group: 'Tools' },
  { key: 'BP_READER_PROGRESSIVE', label: 'Progressive disclosure', default: '1', type: 'bool',
    description: 'Start with only the core ~35-tool surface; agent widens via enable_tool_category. Saves context tokens. On by default.',
    group: 'Tools' },

  // ── Performance ──────────────────────────────────────────────────────────
  { key: 'BP_READER_CACHE_TTL_SECONDS', label: 'Cache TTL (s)', default: '30', type: 'number',
    description: 'How long read-tool responses are memoized. 0 disables caching.',
    group: 'Performance' },
  { key: 'BP_READER_VERBOSE', label: 'Verbose (disable lean mode)', default: '0', type: 'bool',
    description: 'Disables lean-mode response stripping. By default, empty arrays and redundant link data are pruned to save tokens.',
    group: 'Performance' },

  // ── HTTP transport ────────────────────────────────────────────────────────
  { key: 'BP_READER_HTTP_PORT', label: 'HTTP port', default: '', type: 'number',
    description: 'Set to a port (e.g. 7878) to serve JSON-RPC over HTTP/SSE instead of stdio. Binds to 127.0.0.1 only.',
    group: 'HTTP transport' },
  { key: 'BP_READER_HTTP_PATH', label: 'HTTP path', default: '/mcp', type: 'string',
    description: 'URL path for the MCP endpoint when HTTP mode is enabled.',
    group: 'HTTP transport' },
];

const GROUPS = ['Backend', 'Permissions', 'Tools', 'Performance', 'HTTP transport'];

function EditorArgsField({
  value, onChange,
}: { value: string; onChange: (v: string) => void }) {
  // Parse current value into selected flags + free-text remainder
  const selected = new Set(
    EDITOR_ARGS.map((a) => a.flag).filter((f) => value.split(/\s+/).includes(f))
  );
  const extra = value.split(/\s+/).filter((t) => t && !EDITOR_ARGS.some((a) => a.flag === t)).join(' ');

  function toggle(flag: string, mutex?: string[]) {
    const parts = new Set(value.split(/\s+/).filter(Boolean));
    if (parts.has(flag)) {
      parts.delete(flag);
    } else {
      parts.add(flag);
      // Remove mutually exclusive flags
      mutex?.forEach((m) => parts.delete(m));
    }
    onChange(Array.from(parts).join(' '));
  }

  return (
    <div className="space-y-2">
      {EDITOR_ARGS.map((a) => {
        const isChecked = selected.has(a.flag);
        const isMutexBlocked = !isChecked && a.mutex?.some((m) => selected.has(m));
        return (
          <label key={a.flag} className={`flex items-start gap-2 cursor-pointer ${isMutexBlocked ? 'opacity-40' : ''}`}>
            <input
              type="checkbox"
              checked={isChecked}
              disabled={!!isMutexBlocked}
              onChange={() => toggle(a.flag, a.mutex)}
              className="mt-0.5 accent-ue-accent"
            />
            <div>
              <div className="text-xs text-gray-200 font-mono">{a.flag}</div>
              <div className="text-xs text-gray-500">{a.desc}</div>
            </div>
          </label>
        );
      })}
      <div>
        <div className="text-xs text-gray-500 mb-1">Additional flags</div>
        <input
          value={extra}
          onChange={(e) => {
            const parts = new Set(value.split(/\s+/).filter((t) => EDITOR_ARGS.some((a) => a.flag === t)));
            if (e.target.value.trim()) parts.add(e.target.value.trim());
            onChange(Array.from(parts).join(' ') + (e.target.value.trim() ? '' : ''));
            // Just update the free-text portion while preserving checkbox state
            const checkboxPart = Array.from(selected).join(' ');
            onChange((checkboxPart ? checkboxPart + ' ' : '') + e.target.value);
          }}
          placeholder="e.g. -unattended -nosound"
          className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-xs text-gray-200 font-mono focus:border-ue-accent outline-none"
        />
      </div>
    </div>
  );
}

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

  const reset = (key: string, defaultVal: string) => {
    setValues(prev => {
      const next = { ...prev };
      if (defaultVal === '') {
        delete next[key];
      } else {
        next[key] = defaultVal;
      }
      return next;
    });
    setSaved(false);
  };

  const save = () => {
    // Strip entries that match their default (no point storing them)
    const cleaned: Record<string, string> = {};
    for (const [k, v] of Object.entries(values)) {
      const flag = FLAGS.find(f => f.key === k);
      if (v !== '' && v !== (flag?.default ?? '')) cleaned[k] = v;
    }
    setValues(cleaned);
    localStorage.setItem('bpr-env-overrides', JSON.stringify(cleaned));
    setSaved(true);
    setTimeout(() => setSaved(false), 2000);
  };

  const copyEnvBlock = () => {
    const lines = Object.entries(values)
      .filter(([, v]) => v !== '')
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
      <div className="w-40 flex-shrink-0 border-r border-ue-border p-2 space-y-0.5">
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
        <div className="flex items-center justify-between mb-5">
          <h2 className="text-base font-semibold text-white">{activeGroup}</h2>
          <div className="flex gap-2">
            <button onClick={copyEnvBlock}
              className="px-3 py-1.5 text-xs bg-ue-panel border border-ue-border rounded hover:bg-white/5 text-gray-300">
              Copy env block
            </button>
            <button onClick={save}
              className={`px-3 py-1.5 text-xs rounded font-medium transition-colors ${
                saved ? 'bg-green-600 text-white' : 'bg-ue-accent hover:bg-ue-accent-hover text-white'
              }`}>
              {saved ? '✓ Saved' : 'Save'}
            </button>
          </div>
        </div>

        <div className="space-y-3">
          {flags.map(flag => {
            const current = values[flag.key] ?? flag.default;
            const isModified = values[flag.key] !== undefined && values[flag.key] !== flag.default;

            return (
              <div key={flag.key} className={`bg-ue-panel rounded border p-4 ${isModified ? 'border-ue-accent/40' : 'border-ue-border'}`}>
                <div className="flex items-start justify-between gap-4">
                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2 mb-0.5">
                      <span className="text-sm font-medium text-white">{flag.label}</span>
                      {isModified && (
                        <button
                          onClick={() => reset(flag.key, flag.default)}
                          className="text-xs text-ue-accent hover:text-ue-accent-hover"
                          title={`Reset to default: ${flag.default || '(empty)'}`}
                        >
                          ↺ Reset
                        </button>
                      )}
                    </div>
                    <div className="text-xs text-gray-500 font-mono mb-1">{flag.key}</div>
                    <p className="text-xs text-gray-400 leading-relaxed">{flag.description}</p>
                  </div>

                  {flag.type !== 'editorargs' && (
                    <div className="flex-shrink-0 w-48">
                      {flag.type === 'bool' ? (
                        <div className="space-y-1">
                          <select
                            value={current}
                            onChange={e => set(flag.key, e.target.value)}
                            className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white focus:border-ue-accent outline-none"
                          >
                            <option value="1">1 — enabled</option>
                            <option value="0">0 — disabled</option>
                          </select>
                          {!isModified && (
                            <div className="text-xs text-gray-600">default: {flag.default}</div>
                          )}
                        </div>
                      ) : flag.type === 'select' ? (
                        <div className="space-y-1">
                          <select
                            value={current}
                            onChange={e => set(flag.key, e.target.value)}
                            className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white focus:border-ue-accent outline-none"
                          >
                            {flag.options!.map(o => <option key={o} value={o}>{o}</option>)}
                          </select>
                          {!isModified && (
                            <div className="text-xs text-gray-600">default: {flag.default}</div>
                          )}
                        </div>
                      ) : (
                        <div className="space-y-1">
                          <input
                            type={flag.type === 'number' ? 'number' : 'text'}
                            value={current}
                            onChange={e => set(flag.key, e.target.value)}
                            placeholder={flag.default || '(empty)'}
                            className="w-full bg-ue-dark border border-ue-border rounded px-2 py-1.5 text-sm text-white placeholder-gray-600 focus:border-ue-accent outline-none"
                          />
                          {flag.default && !isModified && (
                            <div className="text-xs text-gray-600">default: {flag.default}</div>
                          )}
                        </div>
                      )}
                    </div>
                  )}
                </div>

                {flag.type === 'editorargs' && (
                  <div className="mt-3">
                    <EditorArgsField
                      value={values[flag.key] ?? flag.default}
                      onChange={(v) => set(flag.key, v)}
                    />
                  </div>
                )}
              </div>
            );
          })}
        </div>

        <p className="mt-6 text-xs text-gray-600">
          Settings are stored locally. Use "Copy env block" to paste the JSON snippet into your .mcp.json or AI client config.
        </p>
      </div>
    </div>
  );
}
