import { useState, useEffect, useRef, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import { McpClient, type ToolCallResult } from '../lib/mcp-client';
import JsonViewer from '../components/JsonViewer';
import toolCatalog from '../assets/tools.json';

type Backend = 'mock' | 'commandlet' | 'live' | 'auto';
type TesterMode = 'single' | 'batch';

interface ToolDescriptor {
  name: string;
  description: string;
  category: string;
  input_schema: {
    type: string;
    properties?: Record<string, SchemaProperty>;
    required?: string[];
  };
}

interface SchemaProperty {
  type?: string | string[];
  description?: string;
  enum?: string[];
  items?: SchemaProperty;
}

interface HistoryEntry {
  tool: string;
  args: Record<string, string>;
  result: ToolCallResult;
  elapsed?: number;
  ts: number;
}

// ── Batch types ──────────────────────────────────────────────────────────────
interface BatchStep {
  id: string;
  toolName: string;
  args: Record<string, string>;
  result?: ToolCallResult;
  elapsed?: number;
  status: 'pending' | 'running' | 'done' | 'error';
}

// Resolve {{N.field.path}} templates against prior step results.
// N is the step index (0-based). Path uses dot notation; numeric segments
// index arrays (e.g. {{0.assets.0.asset_path}}).
function resolveTemplate(template: string, results: (ToolCallResult | undefined)[]): string {
  return template.replace(/\{\{(\d+)\.([^}]+)\}\}/g, (match, idxStr, path) => {
    const idx = parseInt(idxStr);
    const result = results[idx];
    if (!result || result.isError) return match;
    const text = result.content?.[0]?.text;
    if (!text) return match;
    try {
      let data: unknown = JSON.parse(text);
      for (const part of path.split('.')) {
        if (data === null || data === undefined) return match;
        if (Array.isArray(data)) {
          const i = parseInt(part);
          data = isNaN(i) ? undefined : data[i];
        } else if (typeof data === 'object') {
          data = (data as Record<string, unknown>)[part];
        } else {
          return match;
        }
      }
      return data === undefined || data === null ? match : String(data);
    } catch {
      return match;
    }
  });
}

function coerceArgs(
  rawArgs: Record<string, string>,
  schema: ToolDescriptor['input_schema'],
  stepResults?: (ToolCallResult | undefined)[]
): Record<string, unknown> {
  const props = schema.properties ?? {};
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(rawArgs)) {
    const resolved = stepResults ? resolveTemplate(v, stepResults) : v;
    if (resolved === '') continue;
    const prop = props[k];
    const t = Array.isArray(prop?.type) ? prop.type[0] : prop?.type;
    if (t === 'boolean') out[k] = resolved === 'true';
    else if (t === 'integer' || t === 'number') out[k] = Number(resolved);
    else out[k] = resolved;
  }
  return out;
}

let _stepIdCounter = 0;
function nextStepId() { return `step-${++_stepIdCounter}`; }

const tools = toolCatalog as unknown as ToolDescriptor[];

function randomPort() { return 7800 + Math.floor(Math.random() * 100); }

// Read the Settings-page env overrides (localStorage['bpr-env-overrides']) and
// strip the keys the Tester UI controls itself, so a stored value can't fight
// the backend/port the user picked here. Everything else (write mode, daemon
// timeouts, tool filters, etc.) flows through to the spawned server.
function readSettingsEnv(): Record<string, string> {
  try {
    const raw = localStorage.getItem('bpr-env-overrides');
    if (!raw) return {};
    const all = JSON.parse(raw) as Record<string, string>;
    delete all['BP_READER_BACKEND'];
    delete all['BP_READER_HTTP_PORT'];
    return all;
  } catch {
    return {};
  }
}

// ── Main component ───────────────────────────────────────────────────────────

export default function Tester() {
  const [backend, setBackend] = useState<Backend>('mock');
  const [serverPid, setServerPid] = useState<number | null>(null);
  const [serverPort, setServerPort] = useState(randomPort());
  const [serverStatus, setServerStatus] = useState<'stopped' | 'starting' | 'running' | 'error'>('stopped');
  const [serverError, setServerError] = useState('');
  const [killing, setKilling] = useState(false);
  const [engineDir, setEngineDir] = useState('');
  const [projectDir, setProjectDir] = useState('');
  const [exePath, setExePath] = useState('');

  // Single mode state
  const [mode, setMode] = useState<TesterMode>('single');
  const [search, setSearch] = useState('');
  const [selectedTool, setSelectedTool] = useState<ToolDescriptor | null>(null);
  const [args, setArgs] = useState<Record<string, string>>({});
  const [sending, setSending] = useState(false);
  const [result, setResult] = useState<ToolCallResult | null>(null);
  const [resultRaw, setResultRaw] = useState('');
  const [showRaw, setShowRaw] = useState(false);

  // Batch mode state
  const [batchSteps, setBatchSteps] = useState<BatchStep[]>([]);
  const [batchRunning, setBatchRunning] = useState(false);
  const [expandedStep, setExpandedStep] = useState<string | null>(null);

  const [history, setHistory] = useState<HistoryEntry[]>([]);
  const [sseEvents, setSseEvents] = useState<string[]>([]);
  const sseRef = useRef<EventSource | null>(null);
  const clientRef = useRef<McpClient | null>(null);

  useEffect(() => {
    bridge.getPaths().then((p) => {
      setEngineDir(p.engineDir);
      setProjectDir(p.projectDir);
      setExePath(p.exePath);
    });
  }, []);

  useEffect(() => {
    if (serverPid === null) return;
    const interval = setInterval(async () => {
      const running = await bridge.isRunning(serverPid);
      if (!running) { setServerStatus('stopped'); setServerPid(null); clientRef.current = null; }
    }, 2000);
    return () => clearInterval(interval);
  }, [serverPid]);

  const filteredTools = search
    ? tools.filter(t => t.name.toLowerCase().includes(search.toLowerCase()) ||
                        t.description.toLowerCase().includes(search.toLowerCase()))
    : tools;
  const categories = Array.from(new Set(tools.map(t => t.category))).sort();

  // ── Server controls ─────────────────────────────────────────────────────

  async function ensureClient(): Promise<McpClient> {
    if (clientRef.current) return clientRef.current;
    if (backend === 'mock') {
      const port = randomPort();
      const pid = await bridge.startServer({ backend: 'mock', port, env: readSettingsEnv() });
      setServerPid(pid); setServerStatus('running'); setServerPort(port);
      const c = new McpClient(port);
      clientRef.current = c;
      return c;
    }
    throw new Error('Server not running — click Start first.');
  }

  async function startServer() {
    setServerStatus('starting'); setServerError('');
    const port = randomPort(); setServerPort(port);
    // Base env = Settings-page overrides; specific paths layered on top.
    const env: Record<string, string> = { ...readSettingsEnv() };
    if (backend !== 'mock') {
      if (engineDir) env['BP_READER_ENGINE_DIR'] = engineDir;
      if (projectDir) env['BP_READER_PROJECT'] =
        `${projectDir}\\${projectDir.split(/[/\\]/).pop()}.uproject`;
    }
    try {
      const pid = await bridge.startServer({ backend, port, env });
      setServerPid(pid); setServerStatus('running');
      clientRef.current = new McpClient(port);
      startSse(port);
    } catch (e) {
      setServerStatus('error');
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  async function stopServer() {
    if (serverPid !== null) { await bridge.stopServer(serverPid); setServerPid(null); setServerStatus('stopped'); }
    sseRef.current?.close(); sseRef.current = null; clientRef.current = null;
  }

  // Terminate any leftover BlueprintReaderMcp.exe + BPR editor daemons (orphan
  // recovery — e.g. after a client crashed without closing its server).
  async function killMcps() {
    setKilling(true);
    const res = await bridge.killMcpServers();
    sseRef.current?.close(); sseRef.current = null; clientRef.current = null;
    setServerPid(null); setServerStatus('stopped');
    if (res.ok) setServerError(`Stopped ${res.count ?? 0} MCP process(es).`);
    else setServerError(res.error ?? 'kill failed');
    setKilling(false);
  }

  function startSse(port: number) {
    sseRef.current?.close();
    const es = new EventSource(`http://127.0.0.1:${port}/mcp`);
    es.onmessage = (ev) => setSseEvents(prev => [...prev.slice(-49), ev.data]);
    es.onerror = () => {};
    sseRef.current = es;
  }

  // ── Shared call executor ────────────────────────────────────────────────

  async function executeCall(
    toolName: string,
    rawArgs: Record<string, string>,
    stepResults?: (ToolCallResult | undefined)[]
  ): Promise<{ result: ToolCallResult; elapsed: number }> {
    const client = await ensureClient();
    const tool = tools.find(t => t.name === toolName);
    if (!tool) throw new Error(`Unknown tool: ${toolName}`);
    const typedArgs = coerceArgs(rawArgs, tool.input_schema, stepResults);
    const t0 = Date.now();
    const r = await client.callTool(toolName, typedArgs);
    return { result: r, elapsed: Date.now() - t0 };
  }

  // ── Single mode ─────────────────────────────────────────────────────────

  function handleToolClick(tool: ToolDescriptor) {
    if (mode === 'batch') {
      // Append as a new batch step
      const step: BatchStep = { id: nextStepId(), toolName: tool.name, args: {}, status: 'pending' };
      setBatchSteps(prev => [...prev, step]);
      setExpandedStep(step.id);
      return;
    }
    setSelectedTool(tool); setArgs({}); setResult(null); setResultRaw('');
  }

  async function sendCall() {
    if (!selectedTool) return;
    setSending(true); setResult(null);
    try {
      const { result: r, elapsed } = await executeCall(selectedTool.name, args);
      setResult(r); setResultRaw(JSON.stringify(r, null, 2));
      setHistory(prev => [{ tool: selectedTool.name, args, result: r, elapsed, ts: Date.now() }, ...prev.slice(0, 19)]);
    } catch (e) {
      const err: ToolCallResult = { content: [{ type: 'text', text: e instanceof Error ? e.message : String(e) }], isError: true };
      setResult(err); setResultRaw(JSON.stringify(err, null, 2));
    } finally {
      setSending(false);
    }
  }

  function loadHistory(entry: HistoryEntry) {
    const tool = tools.find(t => t.name === entry.tool);
    if (tool) { setSelectedTool(tool); setArgs(entry.args); setResult(entry.result); setResultRaw(JSON.stringify(entry.result, null, 2)); }
  }

  const copyResult = useCallback(() => { navigator.clipboard.writeText(resultRaw); }, [resultRaw]);

  // ── Batch mode ──────────────────────────────────────────────────────────

  function updateStep(id: string, patch: Partial<BatchStep>) {
    setBatchSteps(prev => prev.map(s => s.id === id ? { ...s, ...patch } : s));
  }

  function removeStep(id: string) {
    setBatchSteps(prev => prev.filter(s => s.id !== id));
    if (expandedStep === id) setExpandedStep(null);
  }

  function setStepArg(id: string, key: string, value: string) {
    setBatchSteps(prev => prev.map(s =>
      s.id === id ? { ...s, args: { ...s.args, [key]: value } } : s
    ));
  }

  async function runBatch() {
    if (batchSteps.length === 0) return;
    setBatchRunning(true);
    // Reset all steps to pending
    setBatchSteps(prev => prev.map(s => ({ ...s, status: 'pending', result: undefined, elapsed: undefined })));

    const collectedResults: (ToolCallResult | undefined)[] = new Array(batchSteps.length).fill(undefined);

    for (let i = 0; i < batchSteps.length; i++) {
      const step = batchSteps[i];
      updateStep(step.id, { status: 'running' });
      setExpandedStep(step.id);
      try {
        const { result: r, elapsed } = await executeCall(step.toolName, step.args, collectedResults);
        collectedResults[i] = r;
        updateStep(step.id, { status: r.isError ? 'error' : 'done', result: r, elapsed });
      } catch (e) {
        const err: ToolCallResult = {
          content: [{ type: 'text', text: e instanceof Error ? e.message : String(e) }],
          isError: true,
        };
        collectedResults[i] = err;
        updateStep(step.id, { status: 'error', result: err });
        break; // stop on error
      }
    }
    setBatchRunning(false);
  }

  function clearBatch() { setBatchSteps([]); setExpandedStep(null); }

  // ── Render ───────────────────────────────────────────────────────────────

  const statusColors: Record<string, string> = {
    stopped: 'text-gray-400', starting: 'text-yellow-400', running: 'text-green-400', error: 'text-red-400',
  };
  const settingsOverrideCount = Object.keys(readSettingsEnv()).length;

  return (
    <div className="flex h-full">
      {/* Left panel — tool browser */}
      <div className="w-72 flex-shrink-0 border-r border-ue-border flex flex-col">
        {/* Server controls */}
        <div className="p-3 border-b border-ue-border">
          <div className="flex gap-1 mb-2">
            {(['mock', 'commandlet', 'live', 'auto'] as Backend[]).map(b => (
              <button key={b} onClick={() => setBackend(b)}
                className={`flex-1 px-1.5 py-1 rounded text-xs capitalize transition-colors ${
                  backend === b ? 'bg-ue-accent text-white' : 'bg-ue-panel border border-ue-border text-gray-400 hover:bg-white/10'
                }`}>{b}</button>
            ))}
          </div>
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-2">
              <span className={`text-xs ${statusColors[serverStatus]}`}>
                {serverStatus === 'running' ? `PID ${serverPid} :${serverPort}` : serverStatus}
              </span>
              {settingsOverrideCount > 0 && (
                <span className="text-xs text-ue-accent/80" title="Settings-page env overrides applied on start">
                  ⚙ {settingsOverrideCount}
                </span>
              )}
            </div>
            {serverStatus === 'running' ? (
              <button onClick={stopServer} className="px-2 py-0.5 bg-red-600/20 border border-red-500/30 rounded text-xs text-red-400 hover:bg-red-600/30">Stop</button>
            ) : (
              <button onClick={startServer} disabled={serverStatus === 'starting' || !exePath}
                className="px-2 py-0.5 bg-green-600/20 border border-green-500/30 rounded text-xs text-green-400 hover:bg-green-600/30 disabled:opacity-50">
                {serverStatus === 'starting' ? 'Starting…' : 'Start'}
              </button>
            )}
          </div>
          {serverError && <div className="mt-1 text-xs text-red-400 break-all">{serverError}</div>}
          <button
            onClick={killMcps}
            disabled={killing}
            title="Terminate any leftover BlueprintReaderMcp.exe processes and BPR editor daemons (orphan recovery)"
            className="mt-2 w-full px-2 py-1 bg-ue-panel border border-ue-border rounded text-xs text-gray-400 hover:text-red-400 hover:border-red-500/40 disabled:opacity-50"
          >
            {killing ? 'Killing…' : '⦸ Kill all BlueprintReader MCP processes'}
          </button>
        </div>

        {/* Search */}
        <div className="p-3 border-b border-ue-border">
          <input
            className="w-full bg-black/40 border border-ue-border rounded px-3 py-1.5 text-xs text-gray-200 focus:outline-none focus:border-ue-accent"
            placeholder={mode === 'batch' ? 'Click a tool to add to batch…' : 'Search tools…'}
            value={search}
            onChange={e => setSearch(e.target.value)}
          />
        </div>

        {/* Tool list */}
        <div className="flex-1 overflow-auto">
          {search ? (
            filteredTools.slice(0, 50).map(tool => (
              <ToolRow key={tool.name} tool={tool}
                selected={mode === 'single' && selectedTool?.name === tool.name}
                batchMode={mode === 'batch'}
                onSelect={handleToolClick} />
            ))
          ) : (
            categories.map(cat => (
              <div key={cat}>
                <div className="px-3 py-1.5 text-xs text-gray-500 uppercase tracking-wider bg-ue-panel sticky top-0 z-10 border-b border-ue-border/40">
                  {cat}
                </div>
                {tools.filter(t => t.category === cat).map(tool => (
                  <ToolRow key={tool.name} tool={tool}
                    selected={mode === 'single' && selectedTool?.name === tool.name}
                    batchMode={mode === 'batch'}
                    onSelect={handleToolClick} />
                ))}
              </div>
            ))
          )}
        </div>

        {/* History (single mode only) */}
        {mode === 'single' && history.length > 0 && (
          <div className="border-t border-ue-border">
            <div className="px-3 py-1.5 text-xs text-gray-500 uppercase tracking-wider">Recent</div>
            <div className="max-h-28 overflow-auto">
              {history.slice(0, 8).map((h, i) => (
                <button key={i} onClick={() => loadHistory(h)}
                  className="w-full text-left px-3 py-1.5 hover:bg-white/5 flex items-center justify-between gap-2">
                  <span className="text-xs text-gray-300 truncate">{h.tool}</span>
                  {h.result.isError && <span className="text-xs text-red-400">✗</span>}
                </button>
              ))}
            </div>
          </div>
        )}
      </div>

      {/* Right panel */}
      <div className="flex-1 flex flex-col overflow-hidden">
        {/* Mode toggle */}
        <div className="flex items-center gap-2 px-4 py-2 border-b border-ue-border bg-ue-panel/50">
          {(['single', 'batch'] as TesterMode[]).map(m => (
            <button key={m} onClick={() => setMode(m)}
              className={`px-3 py-1 rounded text-xs font-medium capitalize transition-colors ${
                mode === m ? 'bg-ue-accent text-white' : 'text-gray-400 hover:text-gray-200'
              }`}>
              {m === 'batch' ? '⛓ Batch' : '▶ Single'}
            </button>
          ))}
          {mode === 'batch' && batchSteps.length > 0 && (
            <span className="text-xs text-gray-500 ml-1">{batchSteps.length} step{batchSteps.length !== 1 ? 's' : ''}</span>
          )}
        </div>

        {mode === 'single' ? (
          // ── Single call panel ──────────────────────────────────────────
          selectedTool ? (
            <>
              <div className="p-4 border-b border-ue-border">
                <div className="flex items-start justify-between gap-4">
                  <div>
                    <div className="text-sm font-medium text-white">{selectedTool.name}</div>
                    <div className="text-xs text-gray-400 mt-1 leading-relaxed max-w-xl">
                      {selectedTool.description.replace(/^\[.*?\]\s*/, '')}
                    </div>
                  </div>
                  <button onClick={sendCall}
                    disabled={sending || (backend !== 'mock' && serverStatus !== 'running')}
                    className="flex-shrink-0 px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white">
                    {sending ? 'Sending…' : 'Send'}
                  </button>
                </div>
              </div>
              <div className="flex-1 overflow-auto flex">
                <div className="w-64 flex-shrink-0 border-r border-ue-border p-4 overflow-auto">
                  <div className="text-xs text-gray-500 uppercase tracking-wider mb-3">Arguments</div>
                  <ArgForm schema={selectedTool.input_schema} args={args} onChange={(k, v) => setArgs(p => ({ ...p, [k]: v }))} />
                </div>
                <div className="flex-1 p-4 overflow-auto">
                  <div className="flex items-center gap-3 mb-3">
                    <div className="text-xs text-gray-500 uppercase tracking-wider">Response</div>
                    {result && (
                      <>
                        <span className={`text-xs px-2 py-0.5 rounded ${result.isError ? 'bg-red-400/10 text-red-400' : 'bg-green-400/10 text-green-400'}`}>
                          {result.isError ? 'Error' : 'OK'}
                        </span>
                        {result._meta?.elapsed_ms !== undefined && (
                          <span className="text-xs text-gray-500">{result._meta.elapsed_ms}ms</span>
                        )}
                      </>
                    )}
                    <div className="flex-1" />
                    {result && (
                      <>
                        <button onClick={() => setShowRaw(r => !r)} className="text-xs text-gray-500 hover:text-gray-300">{showRaw ? 'Tree' : 'Raw'}</button>
                        <button onClick={copyResult} className="text-xs text-gray-500 hover:text-gray-300">Copy</button>
                      </>
                    )}
                  </div>
                  {result ? (
                    showRaw
                      ? <pre className="text-xs text-gray-300 whitespace-pre-wrap break-all">{resultRaw}</pre>
                      : <div className="select-text text-xs font-mono"><JsonViewer data={result} /></div>
                  ) : (
                    <div className="text-gray-600 text-sm">Press Send to run the tool.</div>
                  )}
                </div>
              </div>
              {sseEvents.length > 0 && (
                <div className="border-t border-ue-border p-3">
                  <div className="text-xs text-gray-500 uppercase tracking-wider mb-2 select-text">SSE Events</div>
                  <div className="max-h-20 overflow-auto space-y-0.5">
                    {sseEvents.map((ev, i) => <div key={i} className="text-xs text-gray-400 font-mono truncate">{ev}</div>)}
                  </div>
                </div>
              )}
            </>
          ) : (
            <div className="flex-1 flex items-center justify-center text-gray-600 text-sm">
              Select a tool from the list.
            </div>
          )
        ) : (
          // ── Batch panel ────────────────────────────────────────────────
          <BatchPanel
            steps={batchSteps}
            running={batchRunning}
            expandedStep={expandedStep}
            onToggleExpand={id => setExpandedStep(p => p === id ? null : id)}
            onSetArg={setStepArg}
            onRemove={removeStep}
            onRunBatch={runBatch}
            onClear={clearBatch}
          />
        )}
      </div>
    </div>
  );
}

// ── ToolRow ──────────────────────────────────────────────────────────────────

function ToolRow({
  tool, selected, batchMode, onSelect,
}: { tool: ToolDescriptor; selected: boolean; batchMode: boolean; onSelect: (t: ToolDescriptor) => void }) {
  return (
    <button onClick={() => onSelect(tool)}
      className={`w-full text-left px-3 py-2 hover:bg-white/5 border-l-2 transition-colors flex items-center justify-between group ${
        selected ? 'border-ue-accent bg-white/5' : 'border-transparent'
      }`}>
      <div className="text-xs text-gray-200 truncate">{tool.name}</div>
      {batchMode && <span className="text-xs text-ue-accent opacity-0 group-hover:opacity-100 flex-shrink-0 ml-1">+</span>}
    </button>
  );
}

// ── ArgForm ──────────────────────────────────────────────────────────────────

function ArgForm({
  schema, args, onChange, stepResults, stepIndex,
}: {
  schema: ToolDescriptor['input_schema'];
  args: Record<string, string>;
  onChange: (key: string, value: string) => void;
  stepResults?: (ToolCallResult | undefined)[];
  stepIndex?: number;
}) {
  const props = schema.properties ?? {};
  const required = new Set(schema.required ?? []);
  const keys = Object.keys(props);

  if (keys.length === 0) return <div className="text-xs text-gray-600">(no arguments)</div>;

  return (
    <div className="space-y-2.5">
      {keys.map(key => {
        const prop = props[key];
        const type = Array.isArray(prop.type) ? prop.type.filter(t => t !== 'null')[0] : prop.type;
        const isRequired = required.has(key);
        const rawValue = args[key] ?? '';
        const hasTemplate = rawValue.includes('{{');
        const resolved = hasTemplate && stepResults ? resolveTemplate(rawValue, stepResults) : null;

        return (
          <div key={key}>
            <label className="block text-xs mb-1">
              <span className="text-gray-300">{key}</span>
              {isRequired && <span className="text-red-400 ml-1">*</span>}
              <span className="text-gray-600 ml-1">({type ?? 'any'})</span>
            </label>
            {prop.enum ? (
              <select value={rawValue} onChange={e => onChange(key, e.target.value)}
                className="w-full bg-black/40 border border-ue-border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent">
                <option value="">— optional —</option>
                {prop.enum.map(v => <option key={v} value={v}>{v}</option>)}
              </select>
            ) : type === 'boolean' ? (
              <select value={rawValue} onChange={e => onChange(key, e.target.value)}
                className="w-full bg-black/40 border border-ue-border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent">
                <option value="">— optional —</option>
                <option value="true">true</option>
                <option value="false">false</option>
              </select>
            ) : (
              <input value={rawValue} onChange={e => onChange(key, e.target.value)}
                placeholder={stepIndex !== undefined && stepIndex > 0
                  ? `value or {{0..${stepIndex - 1}}.field}`
                  : (prop.description?.slice(0, 50) ?? '')}
                className={`w-full bg-black/40 border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent placeholder-gray-700 ${
                  hasTemplate ? 'border-ue-accent/50' : 'border-ue-border'
                }`} />
            )}
            {/* Show resolved template value */}
            {resolved !== null && resolved !== rawValue && (
              <div className="mt-0.5 text-xs text-ue-accent/80 font-mono truncate" title={resolved}>
                → {resolved}
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}

// ── BatchPanel ────────────────────────────────────────────────────────────────

function BatchPanel({
  steps, running, expandedStep, onToggleExpand, onSetArg, onRemove, onRunBatch, onClear,
}: {
  steps: BatchStep[];
  running: boolean;
  expandedStep: string | null;
  onToggleExpand: (id: string) => void;
  onSetArg: (id: string, key: string, value: string) => void;
  onRemove: (id: string) => void;
  onRunBatch: () => void;
  onClear: () => void;
}) {
  const stepResults: (ToolCallResult | undefined)[] = steps.map(s => s.result);

  const stepStatusIcon = (s: BatchStep) => {
    if (s.status === 'running')  return <span className="text-yellow-400 animate-pulse">●</span>;
    if (s.status === 'done')     return <span className="text-green-400">✓</span>;
    if (s.status === 'error')    return <span className="text-red-400">✗</span>;
    return <span className="text-gray-600">○</span>;
  };

  if (steps.length === 0) {
    return (
      <div className="flex-1 flex flex-col items-center justify-center text-center p-8 gap-4">
        <div className="text-gray-500 text-sm max-w-xs">
          Click tools in the list on the left to add them as pipeline steps.
        </div>
        <div className="bg-ue-panel border border-ue-border rounded p-4 text-xs text-gray-500 font-mono text-left max-w-sm">
          <div className="text-gray-400 mb-2">Example pipeline:</div>
          <div>step 0: find_asset</div>
          <div className="text-gray-600 ml-2">name = "BP_Enemy"</div>
          <div className="mt-1">step 1: summarize_blueprint</div>
          <div className="text-gray-600 ml-2 text-ue-accent/70">asset_path = {'{{0.assets.0.asset_path}}'}</div>
          <div className="mt-1">step 2: get_function</div>
          <div className="text-gray-600 ml-2 text-ue-accent/70">asset_path = {'{{0.assets.0.asset_path}}'}</div>
          <div className="text-gray-600 ml-2 text-ue-accent/70">function_name = {'{{1.functions.0.name}}'}</div>
        </div>
        <div className="text-xs text-gray-600">
          Use <code className="text-ue-accent/80">{'{{N.field.path}}'}</code> to pass output from step N to the next step.
          Arrays are indexed with numbers: <code className="text-ue-accent/80">{'{{0.assets.0.name}}'}</code>
        </div>
      </div>
    );
  }

  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      {/* Toolbar */}
      <div className="flex items-center gap-2 px-4 py-2 border-b border-ue-border">
        <button onClick={onRunBatch} disabled={running || steps.length === 0}
          className="px-4 py-1.5 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white flex items-center gap-2">
          {running ? (
            <><span className="animate-spin inline-block">⟳</span> Running…</>
          ) : (
            <>▶ Run Batch</>
          )}
        </button>
        <button onClick={onClear} disabled={running}
          className="px-3 py-1.5 bg-ue-panel border border-ue-border rounded text-xs text-gray-400 hover:bg-white/10 disabled:opacity-50">
          Clear
        </button>
        <div className="flex-1" />
        <div className="text-xs text-gray-600">
          <code className="text-gray-500">{'{{N.field}}'}</code> references step N's output
        </div>
      </div>

      {/* Steps */}
      <div className="flex-1 overflow-auto p-3 space-y-2">
        {steps.map((step, idx) => {
          const tool = tools.find(t => t.name === step.toolName);
          const isExpanded = expandedStep === step.id;
          const resultText = step.result?.content?.[0]?.text;
          let resultPreview = '';
          if (resultText) {
            try {
              const parsed = JSON.parse(resultText);
              resultPreview = JSON.stringify(parsed, null, 0).slice(0, 120);
            } catch { resultPreview = resultText.slice(0, 120); }
          }

          return (
            <div key={step.id} className={`border rounded transition-colors ${
              step.status === 'running' ? 'border-yellow-500/40' :
              step.status === 'done'    ? 'border-green-500/30' :
              step.status === 'error'   ? 'border-red-500/30' :
              'border-ue-border'
            } bg-ue-panel`}>
              {/* Step header */}
              <div className="flex items-center gap-2 px-3 py-2 cursor-pointer hover:bg-white/5"
                onClick={() => onToggleExpand(step.id)}>
                <span className="text-xs text-gray-500 w-5 text-center font-mono">{idx}</span>
                {stepStatusIcon(step)}
                <span className="text-sm text-white font-medium flex-1 truncate">{step.toolName}</span>
                {step.elapsed !== undefined && (
                  <span className="text-xs text-gray-600">{step.elapsed}ms</span>
                )}
                <button onClick={e => { e.stopPropagation(); onRemove(step.id); }}
                  className="text-gray-600 hover:text-red-400 text-xs w-5">✕</button>
                <span className={`text-gray-500 text-xs transition-transform ${isExpanded ? 'rotate-180' : ''}`}>▾</span>
              </div>

              {/* Collapsed result preview */}
              {!isExpanded && resultPreview && (
                <div className="px-3 pb-2 text-xs text-gray-500 font-mono truncate">{resultPreview}</div>
              )}

              {/* Expanded detail */}
              {isExpanded && tool && (
                <div className="border-t border-ue-border/50 flex">
                  {/* Args */}
                  <div className="w-64 flex-shrink-0 p-3 border-r border-ue-border/50">
                    <div className="text-xs text-gray-500 uppercase tracking-wider mb-2">Arguments</div>
                    <ArgForm
                      schema={tool.input_schema}
                      args={step.args}
                      onChange={(k, v) => onSetArg(step.id, k, v)}
                      stepResults={stepResults}
                      stepIndex={idx}
                    />
                  </div>
                  {/* Result */}
                  <div className="flex-1 p-3 overflow-auto max-h-64">
                    {step.result ? (
                      <>
                        <div className="flex items-center gap-2 mb-2">
                          <span className="text-xs text-gray-500 uppercase tracking-wider">Result</span>
                          <span className={`text-xs px-1.5 py-0.5 rounded ${step.result.isError ? 'bg-red-400/10 text-red-400' : 'bg-green-400/10 text-green-400'}`}>
                            {step.result.isError ? 'Error' : 'OK'}
                          </span>
                          <button onClick={() => { try { navigator.clipboard.writeText(JSON.stringify(step.result, null, 2)); } catch {} }}
                            className="text-xs text-gray-600 hover:text-gray-400 ml-auto">Copy</button>
                        </div>
                        <pre className="text-xs text-gray-300 whitespace-pre-wrap break-all">
                          {step.result.content?.[0]?.text ?? JSON.stringify(step.result)}
                        </pre>
                      </>
                    ) : (
                      <div className="text-xs text-gray-600 pt-4 text-center">
                        {step.status === 'pending' ? 'Waiting to run…' : 'Running…'}
                      </div>
                    )}
                  </div>
                </div>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}
