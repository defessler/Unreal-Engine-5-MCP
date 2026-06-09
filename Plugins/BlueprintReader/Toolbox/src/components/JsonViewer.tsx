import { useState } from 'react';

// TBX-R7: large tool results (big BP/graph dumps) used to mount a component +
// useState per node with no bounds, freezing the renderer. Now each level caps
// how many children it renders ("show more" reveals the rest on demand), deep
// levels start collapsed (lazy child render), and long strings truncate.

interface JsonViewerProps {
  data: unknown;
  depth?: number;
}

const ITEM_CAP = 100;       // children rendered per level before "show more"
const STRING_LIMIT = 1000;  // chars before a string truncates

export default function JsonViewer({ data, depth = 0 }: JsonViewerProps) {
  if (data === null) return <span className="text-gray-500">null</span>;
  if (typeof data === 'boolean') return <span className="text-yellow-400">{String(data)}</span>;
  if (typeof data === 'number') return <span className="text-blue-400">{String(data)}</span>;
  if (typeof data === 'string') return <StringNode value={data} />;

  if (Array.isArray(data)) {
    return <ArrayNode data={data} depth={depth} />;
  }
  if (typeof data === 'object') {
    return <ObjectNode data={data as Record<string, unknown>} depth={depth} />;
  }
  return <span className="text-gray-300">{String(data)}</span>;
}

function StringNode({ value }: { value: string }) {
  const [expanded, setExpanded] = useState(false);
  if (value.length <= STRING_LIMIT) return <span className="text-green-400">"{value}"</span>;
  return (
    <span className="text-green-400">
      "{expanded ? value : value.slice(0, STRING_LIMIT)}"
      <button
        onClick={() => setExpanded(!expanded)}
        className="ml-1 text-xs text-gray-400 hover:text-white"
      >
        {expanded ? '(less)' : `… (+${(value.length - STRING_LIMIT).toLocaleString()} chars)`}
      </button>
    </span>
  );
}

function ObjectNode({ data, depth }: { data: Record<string, unknown>; depth: number }) {
  const [collapsed, setCollapsed] = useState(depth > 2);
  const [shown, setShown] = useState(ITEM_CAP);
  const keys = Object.keys(data);
  if (keys.length === 0) return <span className="text-gray-500">{'{}'}</span>;
  // Deep/collapsed nodes don't render children at all until expanded (R7).
  const visible = keys.slice(0, shown);

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
            {visible.map((k) => (
              <div key={k}>
                <span className="text-purple-400">"{k}"</span>
                <span className="text-gray-400">: </span>
                <JsonViewer data={data[k]} depth={depth + 1} />
                <span className="text-gray-600">,</span>
              </div>
            ))}
            {keys.length > shown && (
              <button
                onClick={() => setShown((n) => n + ITEM_CAP)}
                className="text-xs text-ue-accent hover:underline"
              >
                show {Math.min(ITEM_CAP, keys.length - shown)} more of {keys.length} keys…
              </button>
            )}
          </div>
          {'}'}
        </span>
      )}
    </span>
  );
}

function ArrayNode({ data, depth }: { data: unknown[]; depth: number }) {
  const [collapsed, setCollapsed] = useState(depth > 2);
  const [shown, setShown] = useState(ITEM_CAP);
  if (data.length === 0) return <span className="text-gray-500">{'[]'}</span>;
  const visible = data.slice(0, shown);

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
            {visible.map((item, i) => (
              <div key={i}>
                <JsonViewer data={item} depth={depth + 1} />
                {i < data.length - 1 && <span className="text-gray-600">,</span>}
              </div>
            ))}
            {data.length > shown && (
              <button
                onClick={() => setShown((n) => n + ITEM_CAP)}
                className="text-xs text-ue-accent hover:underline"
              >
                show {Math.min(ITEM_CAP, data.length - shown)} more of {data.length} items…
              </button>
            )}
          </div>
          {']'}
        </span>
      )}
    </span>
  );
}
