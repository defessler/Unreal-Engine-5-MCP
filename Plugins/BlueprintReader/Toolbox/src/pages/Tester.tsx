import { useState, useEffect, useRef, useCallback } from 'react';
import { bridge } from '../lib/bridge';
import { McpClient, type ToolCallResult } from '../lib/mcp-client';
import JsonViewer from '../components/JsonViewer';
import toolCatalog from '../assets/tools.json';

type Backend = 'mock' | 'commandlet' | 'live' | 'auto';

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

const tools = toolCatalog as ToolDescriptor[];

function randomPort() {
  return 7800 + Math.floor(Math.random() * 100);
}

export default function Tester() {
  const [backend, setBackend] = useState<Backend>('mock');
  const [serverPid, setServerPid] = useState<number | null>(null);
  const [serverPort, setServerPort] = useState(randomPort());
  const [serverStatus, setServerStatus] = useState<'stopped' | 'starting' | 'running' | 'error'>('stopped');
  const [serverError, setServerError] = useState('');
  const [engineDir, setEngineDir] = useState('');
  const [projectDir, setProjectDir] = useState('');
  const [exePath, setExePath] = useState('');

  const [search, setSearch] = useState('');
  const [selectedTool, setSelectedTool] = useState<ToolDescriptor | null>(null);
  const [args, setArgs] = useState<Record<string, string>>({});
  const [sending, setSending] = useState(false);
  const [result, setResult] = useState<ToolCallResult | null>(null);
  const [resultRaw, setResultRaw] = useState('');
  const [showRaw, setShowRaw] = useState(false);

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

  // Poll server status
  useEffect(() => {
    if (serverPid === null) return;
    const interval = setInterval(async () => {
      const running = await bridge.isRunning(serverPid);
      if (!running) {
        setServerStatus('stopped');
        setServerPid(null);
        clientRef.current = null;
      }
    }, 2000);
    return () => clearInterval(interval);
  }, [serverPid]);

  const filteredTools = search
    ? tools.filter(
        (t) =>
          t.name.toLowerCase().includes(search.toLowerCase()) ||
          t.description.toLowerCase().includes(search.toLowerCase())
      )
    : tools;

  const categories = Array.from(new Set(tools.map((t) => t.category))).sort();

  async function startServer() {
    setServerStatus('starting');
    setServerError('');
    const port = randomPort();
    setServerPort(port);

    const env: Record<string, string> = {};
    if (backend !== 'mock') {
      if (engineDir) env['BP_READER_ENGINE_DIR'] = engineDir;
      if (projectDir) env['BP_READER_PROJECT'] = `${projectDir}\\${projectDir.split(/[/\\]/).pop()}.uproject`;
    }

    try {
      const pid = await bridge.startServer({ backend, port, env });
      setServerPid(pid);
      setServerStatus('running');
      clientRef.current = new McpClient(port);
      startSse(port);
    } catch (e) {
      setServerStatus('error');
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  async function stopServer() {
    if (serverPid !== null) {
      await bridge.stopServer(serverPid);
      setServerPid(null);
      setServerStatus('stopped');
    }
    sseRef.current?.close();
    sseRef.current = null;
    clientRef.current = null;
  }

  function startSse(port: number) {
    sseRef.current?.close();
    const es = new EventSource(`http://127.0.0.1:${port}/mcp`);
    es.onmessage = (ev) => {
      setSseEvents((prev) => [...prev.slice(-49), ev.data]);
    };
    es.onerror = () => {
      // SSE errors are normal when server isn't running
    };
    sseRef.current = es;
  }

  function selectTool(tool: ToolDescriptor) {
    setSelectedTool(tool);
    setArgs({});
    setResult(null);
    setResultRaw('');
  }

  function setArg(key: string, value: string) {
    setArgs((prev) => ({ ...prev, [key]: value }));
  }

  async function sendCall() {
    if (!selectedTool) return;
    if (backend !== 'mock' && serverStatus !== 'running') {
      alert('Start the server first.');
      return;
    }

    setSending(true);
    setResult(null);

    try {
      let client = clientRef.current;
      if (!client) {
        if (backend === 'mock') {
          // For mock, start an ephemeral server
          const port = randomPort();
          const pid = await bridge.startServer({ backend: 'mock', port, env: {} });
          setServerPid(pid);
          setServerStatus('running');
          setServerPort(port);
          client = new McpClient(port);
          clientRef.current = client;
        } else {
          throw new Error('Server not running. Click Start first.');
        }
      }

      // Convert string args to appropriate types
      const typedArgs: Record<string, unknown> = {};
      const props = selectedTool.input_schema.properties ?? {};
      for (const [k, v] of Object.entries(args)) {
        if (v === '') continue;
        const prop = props[k];
        const t = Array.isArray(prop?.type) ? prop.type[0] : prop?.type;
        if (t === 'boolean') typedArgs[k] = v === 'true';
        else if (t === 'integer' || t === 'number') typedArgs[k] = Number(v);
        else typedArgs[k] = v;
      }

      const t0 = Date.now();
      const r = await client.callTool(selectedTool.name, typedArgs);
      const elapsed = Date.now() - t0;

      setResult(r);
      setResultRaw(JSON.stringify(r, null, 2));

      const entry: HistoryEntry = { tool: selectedTool.name, args, result: r, elapsed, ts: Date.now() };
      setHistory((prev) => [entry, ...prev.slice(0, 19)]);
    } catch (e) {
      const errResult: ToolCallResult = {
        content: [{ type: 'text', text: e instanceof Error ? e.message : String(e) }],
        isError: true,
      };
      setResult(errResult);
      setResultRaw(JSON.stringify(errResult, null, 2));
    } finally {
      setSending(false);
    }
  }

  function loadHistory(entry: HistoryEntry) {
    const tool = tools.find((t) => t.name === entry.tool);
    if (tool) {
      setSelectedTool(tool);
      setArgs(entry.args);
      setResult(entry.result);
      setResultRaw(JSON.stringify(entry.result, null, 2));
    }
  }

  const copyResult = useCallback(() => {
    navigator.clipboard.writeText(resultRaw);
  }, [resultRaw]);

  const statusColors: Record<typeof serverStatus, string> = {
    stopped: 'text-gray-400',
    starting: 'text-yellow-400',
    running: 'text-green-400',
    error: 'text-red-400',
  };

  return (
    <div className="flex h-full">
      {/* Left panel */}
      <div className="w-80 flex-shrink-0 border-r border-ue-border flex flex-col">
        {/* Backend + server controls */}
        <div className="p-4 border-b border-ue-border">
          <div className="flex gap-1 mb-3">
            {(['mock', 'commandlet', 'live', 'auto'] as Backend[]).map((b) => (
              <button
                key={b}
                onClick={() => setBackend(b)}
                className={`flex-1 px-2 py-1 rounded text-xs capitalize transition-colors ${
                  backend === b
                    ? 'bg-ue-accent text-white'
                    : 'bg-ue-panel border border-ue-border text-gray-400 hover:bg-white/10'
                }`}
              >
                {b}
              </button>
            ))}
          </div>
          <div className="flex items-center justify-between">
            <div className={`text-xs ${statusColors[serverStatus]}`}>
              {serverStatus === 'running' ? `PID ${serverPid} :${serverPort}` : serverStatus}
            </div>
            {serverStatus === 'running' ? (
              <button
                onClick={stopServer}
                className="px-3 py-1 bg-red-600/20 border border-red-500/30 rounded text-xs text-red-400 hover:bg-red-600/30"
              >
                Stop
              </button>
            ) : (
              <button
                onClick={startServer}
                disabled={serverStatus === 'starting' || !exePath}
                className="px-3 py-1 bg-green-600/20 border border-green-500/30 rounded text-xs text-green-400 hover:bg-green-600/30 disabled:opacity-50"
              >
                {serverStatus === 'starting' ? 'Starting…' : 'Start'}
              </button>
            )}
          </div>
          {serverError && <div className="mt-2 text-xs text-red-400 break-all">{serverError}</div>}
        </div>

        {/* Tool search */}
        <div className="p-3 border-b border-ue-border">
          <input
            className="w-full bg-black/40 border border-ue-border rounded px-3 py-1.5 text-xs text-gray-200 focus:outline-none focus:border-ue-accent"
            placeholder="Search 258 tools…"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
          />
        </div>

        {/* Tool list */}
        <div className="flex-1 overflow-auto">
          {search ? (
            filteredTools.slice(0, 50).map((tool) => (
              <ToolRow key={tool.name} tool={tool} selected={selectedTool?.name === tool.name} onSelect={selectTool} />
            ))
          ) : (
            categories.map((cat) => (
              <div key={cat}>
                <div className="px-3 py-1.5 text-xs text-gray-600 uppercase tracking-wider bg-black/20 sticky top-0">
                  {cat}
                </div>
                {tools
                  .filter((t) => t.category === cat)
                  .map((tool) => (
                    <ToolRow key={tool.name} tool={tool} selected={selectedTool?.name === tool.name} onSelect={selectTool} />
                  ))}
              </div>
            ))
          )}
        </div>

        {/* History */}
        {history.length > 0 && (
          <div className="border-t border-ue-border">
            <div className="px-3 py-2 text-xs text-gray-500 uppercase tracking-wider">Recent</div>
            <div className="max-h-32 overflow-auto">
              {history.slice(0, 10).map((h, i) => (
                <button
                  key={i}
                  onClick={() => loadHistory(h)}
                  className="w-full text-left px-3 py-1.5 hover:bg-white/5 flex items-center justify-between gap-2"
                >
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
        {selectedTool ? (
          <>
            {/* Tool header */}
            <div className="p-4 border-b border-ue-border">
              <div className="flex items-start justify-between gap-4">
                <div>
                  <div className="text-sm font-medium text-white">{selectedTool.name}</div>
                  <div className="text-xs text-gray-400 mt-1 leading-relaxed max-w-xl">
                    {selectedTool.description.replace(/^\[.*?\]\s*/, '')}
                  </div>
                </div>
                <button
                  onClick={sendCall}
                  disabled={sending || (backend !== 'mock' && serverStatus !== 'running')}
                  className="flex-shrink-0 px-4 py-2 bg-ue-accent hover:bg-ue-accent-hover disabled:opacity-50 rounded text-sm font-medium text-white"
                >
                  {sending ? 'Sending…' : 'Send'}
                </button>
              </div>
            </div>

            <div className="flex-1 overflow-auto flex">
              {/* Args */}
              <div className="w-72 flex-shrink-0 border-r border-ue-border p-4 overflow-auto">
                <div className="text-xs text-gray-500 uppercase tracking-wider mb-3">Arguments</div>
                <ArgForm schema={selectedTool.input_schema} args={args} onChange={setArg} />
              </div>

              {/* Response */}
              <div className="flex-1 p-4 overflow-auto">
                <div className="flex items-center gap-3 mb-3">
                  <div className="text-xs text-gray-500 uppercase tracking-wider">Response</div>
                  {result && (
                    <>
                      <span
                        className={`text-xs px-2 py-0.5 rounded ${
                          result.isError
                            ? 'bg-red-400/10 text-red-400'
                            : 'bg-green-400/10 text-green-400'
                        }`}
                      >
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
                      <button
                        onClick={() => setShowRaw(!showRaw)}
                        className="text-xs text-gray-500 hover:text-gray-300"
                      >
                        {showRaw ? 'Tree' : 'Raw'}
                      </button>
                      <button onClick={copyResult} className="text-xs text-gray-500 hover:text-gray-300">
                        Copy
                      </button>
                    </>
                  )}
                </div>

                {result ? (
                  showRaw ? (
                    <pre className="text-xs text-gray-300 whitespace-pre-wrap break-all">{resultRaw}</pre>
                  ) : (
                    <div className="text-xs font-mono">
                      <JsonViewer data={result} />
                    </div>
                  )
                ) : (
                  <div className="text-gray-600 text-sm">Press Send to run the tool.</div>
                )}
              </div>
            </div>

            {/* SSE events */}
            {sseEvents.length > 0 && (
              <div className="border-t border-ue-border p-3">
                <div className="text-xs text-gray-500 uppercase tracking-wider mb-2">SSE Events</div>
                <div className="max-h-24 overflow-auto space-y-0.5">
                  {sseEvents.map((ev, i) => (
                    <div key={i} className="text-xs text-gray-400 font-mono truncate">{ev}</div>
                  ))}
                </div>
              </div>
            )}
          </>
        ) : (
          <div className="flex-1 flex items-center justify-center text-gray-600">
            Select a tool from the list to get started.
          </div>
        )}
      </div>
    </div>
  );
}

function ToolRow({
  tool,
  selected,
  onSelect,
}: {
  tool: ToolDescriptor;
  selected: boolean;
  onSelect: (t: ToolDescriptor) => void;
}) {
  return (
    <button
      onClick={() => onSelect(tool)}
      className={`w-full text-left px-3 py-2 hover:bg-white/5 border-l-2 transition-colors ${
        selected ? 'border-ue-accent bg-white/5' : 'border-transparent'
      }`}
    >
      <div className="text-xs text-gray-200 truncate">{tool.name}</div>
    </button>
  );
}

function ArgForm({
  schema,
  args,
  onChange,
}: {
  schema: ToolDescriptor['input_schema'];
  args: Record<string, string>;
  onChange: (key: string, value: string) => void;
}) {
  const props = schema.properties ?? {};
  const required = new Set(schema.required ?? []);
  const keys = Object.keys(props);

  if (keys.length === 0) {
    return <div className="text-xs text-gray-600">(no arguments)</div>;
  }

  return (
    <div className="space-y-3">
      {keys.map((key) => {
        const prop = props[key];
        const type = Array.isArray(prop.type) ? prop.type.filter((t) => t !== 'null')[0] : prop.type;
        const isRequired = required.has(key);
        return (
          <div key={key}>
            <label className="block text-xs mb-1">
              <span className="text-gray-300">{key}</span>
              {isRequired && <span className="text-red-400 ml-1">*</span>}
              <span className="text-gray-600 ml-1">({type ?? 'any'})</span>
            </label>
            {prop.enum ? (
              <select
                value={args[key] ?? ''}
                onChange={(e) => onChange(key, e.target.value)}
                className="w-full bg-black/40 border border-ue-border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent"
              >
                <option value="">— optional —</option>
                {prop.enum.map((v) => (
                  <option key={v} value={v}>{v}</option>
                ))}
              </select>
            ) : type === 'boolean' ? (
              <select
                value={args[key] ?? ''}
                onChange={(e) => onChange(key, e.target.value)}
                className="w-full bg-black/40 border border-ue-border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent"
              >
                <option value="">— optional —</option>
                <option value="true">true</option>
                <option value="false">false</option>
              </select>
            ) : (
              <input
                value={args[key] ?? ''}
                onChange={(e) => onChange(key, e.target.value)}
                placeholder={prop.description?.slice(0, 50) ?? ''}
                className="w-full bg-black/40 border border-ue-border rounded px-2 py-1 text-xs text-gray-200 focus:outline-none focus:border-ue-accent placeholder-gray-700"
              />
            )}
          </div>
        );
      })}
    </div>
  );
}
