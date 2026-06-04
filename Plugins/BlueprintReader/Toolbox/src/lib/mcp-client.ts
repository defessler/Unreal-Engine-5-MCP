// Typed MCP HTTP client (POST /mcp for requests, EventSource GET /mcp for SSE).
// Handles the initialize handshake automatically before the first tools/call.

let nextId = 1;

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

export class McpClient {
  private baseUrl: string;
  private sessionId: string | null = null;
  private initialized = false;

  constructor(port: number) {
    this.baseUrl = `http://127.0.0.1:${port}/mcp`;
  }

  private async post(method: string, params: unknown): Promise<unknown> {
    const req: JsonRpcRequest = {
      jsonrpc: '2.0',
      id: nextId++,
      method,
      params,
    };

    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
    };
    if (this.sessionId) {
      headers['Mcp-Session-Id'] = this.sessionId;
    }

    const resp = await fetch(this.baseUrl, {
      method: 'POST',
      headers,
      body: JSON.stringify(req),
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
  }

  async initialize(): Promise<void> {
    if (this.initialized) return;
    await this.post('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'BlueprintReader Toolbox', version: '1.0.0' },
    });
    this.initialized = true;
  }

  async listTools(): Promise<{ tools: Array<{ name: string; description: string; inputSchema: unknown }> }> {
    await this.initialize();
    return this.post('tools/list', {}) as Promise<{ tools: Array<{ name: string; description: string; inputSchema: unknown }> }>;
  }

  async callTool(name: string, args: Record<string, unknown>): Promise<ToolCallResult> {
    await this.initialize();
    const result = await this.post('tools/call', { name, arguments: args });
    return result as ToolCallResult;
  }

  openSseStream(onEvent: (event: MessageEvent) => void): EventSource {
    const url = this.sessionId
      ? `${this.baseUrl}?session=${this.sessionId}`
      : this.baseUrl;
    const es = new EventSource(url);
    es.onmessage = onEvent;
    return es;
  }

  reset() {
    this.sessionId = null;
    this.initialized = false;
  }
}
