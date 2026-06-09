// Typed MCP HTTP client (POST /mcp for requests, EventSource GET /mcp for SSE).
// Handles the initialize handshake automatically before the first tools/call.
//
// TBX-F6: every request has a timeout + is cancellable (AbortController); the
// initialize handshake is de-raced via a cached in-flight promise; the JSON-RPC
// id counter is per-instance (not module-global); a lost session transparently
// re-initializes once. The daemon is known-slow/flaky, so the client must time
// out and recover rather than hang the UI forever.

interface JsonRpcRequest {
  jsonrpc: '2.0';
  id: number;
  method: string;
  params: unknown;
}

interface JsonRpcResponse {
  jsonrpc: '2.0';
  id: number;
  result?: unknown;
  error?: { code: number; message: string };
}

export interface ToolCallResult {
  content: Array<{ type: string; text?: string }>;
  isError?: boolean;
  _meta?: { elapsed_ms: number; tool: string };
}

export interface RequestOpts {
  /** External cancel signal (e.g. a Tester "Cancel" button). */
  signal?: AbortSignal;
  /** Per-request timeout override; falls back to the client default. 0 disables. */
  timeoutMs?: number;
}

const DEFAULT_TIMEOUT_MS = 30_000;

export class McpClient {
  private baseUrl: string;
  private sessionId: string | null = null;
  private initialized = false;
  private initPromise: Promise<void> | null = null;
  private nextId = 1;
  private inFlight = new Set<AbortController>();
  readonly defaultTimeoutMs: number;

  constructor(port: number, opts?: { timeoutMs?: number }) {
    this.baseUrl = `http://127.0.0.1:${port}/mcp`;
    this.defaultTimeoutMs = opts?.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  }

  private async post(method: string, params: unknown, opts?: RequestOpts): Promise<unknown> {
    const req: JsonRpcRequest = {
      jsonrpc: '2.0',
      id: this.nextId++,
      method,
      params,
    };

    const headers: Record<string, string> = { 'Content-Type': 'application/json' };
    if (this.sessionId) headers['Mcp-Session-Id'] = this.sessionId;

    // One controller per request. The timer aborts on timeout; an external
    // signal (cancel button / cancelAll) aborts on user request. We track which
    // fired so the error message is accurate.
    const ac = new AbortController();
    this.inFlight.add(ac);
    let timedOut = false;
    const timeoutMs = opts?.timeoutMs ?? this.defaultTimeoutMs;
    const timer =
      timeoutMs > 0
        ? setTimeout(() => { timedOut = true; ac.abort(); }, timeoutMs)
        : null;
    if (opts?.signal) {
      if (opts.signal.aborted) ac.abort();
      else opts.signal.addEventListener('abort', () => ac.abort(), { once: true });
    }

    try {
      const resp = await fetch(this.baseUrl, {
        method: 'POST',
        headers,
        body: JSON.stringify(req),
        signal: ac.signal,
      });

      if (!resp.ok) {
        throw new Error(`HTTP ${resp.status}: ${resp.statusText}`);
      }

      const sid = resp.headers.get('Mcp-Session-Id');
      if (sid) this.sessionId = sid;

      const body = (await resp.json()) as JsonRpcResponse;
      if (body.error) {
        throw new Error(`JSON-RPC error ${body.error.code}: ${body.error.message}`);
      }
      return body.result;
    } catch (err) {
      if (ac.signal.aborted) {
        throw new Error(
          timedOut
            ? `Request timed out after ${timeoutMs} ms (the editor/daemon may be busy or unreachable).`
            : 'Request cancelled.',
        );
      }
      throw err;
    } finally {
      if (timer) clearTimeout(timer);
      this.inFlight.delete(ac);
    }
  }

  /** ensureInitialized → post, with a single transparent retry on session loss. */
  private async request(method: string, params: unknown, opts?: RequestOpts): Promise<unknown> {
    await this.initialize(opts);
    try {
      return await this.post(method, params, opts);
    } catch (err) {
      if (this.isSessionLost(err)) {
        this.reset();
        await this.initialize(opts);
        return this.post(method, params, opts);
      }
      throw err;
    }
  }

  private isSessionLost(err: unknown): boolean {
    const m = err instanceof Error ? err.message.toLowerCase() : '';
    return (
      m.includes('session') &&
      (m.includes('not found') || m.includes('unknown') || m.includes('expired') || m.includes('-32001'))
    ) || m.includes('http 404');
  }

  /**
   * Idempotent, race-free handshake. Concurrent callers share one in-flight
   * promise so a burst of tool calls can't fire two `initialize` requests.
   */
  async initialize(opts?: RequestOpts): Promise<void> {
    if (this.initialized) return;
    if (!this.initPromise) this.initPromise = this.doInitialize(opts);
    try {
      await this.initPromise;
    } catch (err) {
      this.initPromise = null; // allow a later retry after a failed handshake
      throw err;
    }
  }

  private async doInitialize(opts?: RequestOpts): Promise<void> {
    await this.post('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'BlueprintReader Toolbox', version: '1.0.0' },
    }, opts);
    this.initialized = true;
  }

  async listTools(opts?: RequestOpts): Promise<{ tools: Array<{ name: string; description: string; inputSchema: unknown }> }> {
    return this.request('tools/list', {}, opts) as Promise<{
      tools: Array<{ name: string; description: string; inputSchema: unknown }>;
    }>;
  }

  async callTool(name: string, args: Record<string, unknown>, opts?: RequestOpts): Promise<ToolCallResult> {
    return this.request('tools/call', { name, arguments: args }, opts) as Promise<ToolCallResult>;
  }

  openSseStream(onEvent: (event: MessageEvent) => void): EventSource {
    const url = this.sessionId
      ? `${this.baseUrl}?session=${this.sessionId}`
      : this.baseUrl;
    const es = new EventSource(url);
    es.onmessage = onEvent;
    return es;
  }

  /** Abort every in-flight request (e.g. the user hit Cancel or closed the page). */
  cancelAll(): void {
    for (const ac of this.inFlight) ac.abort();
    this.inFlight.clear();
  }

  reset(): void {
    this.sessionId = null;
    this.initialized = false;
    this.initPromise = null;
  }
}
