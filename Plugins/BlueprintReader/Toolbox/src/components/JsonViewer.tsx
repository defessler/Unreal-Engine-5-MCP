import { useState } from 'react';

interface JsonViewerProps {
  data: unknown;
  depth?: number;
}

export default function JsonViewer({ data, depth = 0 }: JsonViewerProps) {
  if (data === null) return <span className="text-gray-500">null</span>;
  if (typeof data === 'boolean') return <span className="text-yellow-400">{String(data)}</span>;
  if (typeof data === 'number') return <span className="text-blue-400">{String(data)}</span>;
  if (typeof data === 'string') return <span className="text-green-400">"{data}"</span>;

  if (Array.isArray(data)) {
    return <ArrayNode data={data} depth={depth} />;
  }
  if (typeof data === 'object') {
    return <ObjectNode data={data as Record<string, unknown>} depth={depth} />;
  }
  return <span className="text-gray-300">{String(data)}</span>;
}

function ObjectNode({ data, depth }: { data: Record<string, unknown>; depth: number }) {
  const [collapsed, setCollapsed] = useState(depth > 2);
  const keys = Object.keys(data);
  if (keys.length === 0) return <span className="text-gray-500">{'{}'}</span>;

  return (
    <span>
      <button
        onClick={() => setCollapsed(!collapsed)}
        className="text-gray-400 hover:text-white mr-1 text-xs"
      >
        {collapsed ? '▶' : '▼'}
      </button>
      {collapsed ? (
        <span
          className="text-gray-400 cursor-pointer hover:text-white"
          onClick={() => setCollapsed(false)}
        >
          {`{ ${keys.length} keys }`}
        </span>
      ) : (
        <span>
          {'{'}
          <div className="ml-4">
            {keys.map((k) => (
              <div key={k}>
                <span className="text-purple-400">"{k}"</span>
                <span className="text-gray-400">: </span>
                <JsonViewer data={data[k]} depth={depth + 1} />
                <span className="text-gray-600">,</span>
              </div>
            ))}
          </div>
          {'}'}
        </span>
      )}
    </span>
  );
}

function ArrayNode({ data, depth }: { data: unknown[]; depth: number }) {
  const [collapsed, setCollapsed] = useState(depth > 2);
  if (data.length === 0) return <span className="text-gray-500">{'[]'}</span>;

  return (
    <span>
      <button
        onClick={() => setCollapsed(!collapsed)}
        className="text-gray-400 hover:text-white mr-1 text-xs"
      >
        {collapsed ? '▶' : '▼'}
      </button>
      {collapsed ? (
        <span
          className="text-gray-400 cursor-pointer hover:text-white"
          onClick={() => setCollapsed(false)}
        >
          {`[ ${data.length} items ]`}
        </span>
      ) : (
        <span>
          {'['}
          <div className="ml-4">
            {data.map((item, i) => (
              <div key={i}>
                <JsonViewer data={item} depth={depth + 1} />
                {i < data.length - 1 && <span className="text-gray-600">,</span>}
              </div>
            ))}
          </div>
          {']'}
        </span>
      )}
    </span>
  );
}
