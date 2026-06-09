import { useEffect, useRef } from 'react';

interface LogStreamProps {
  lines: string[];
  maxHeight?: string;
}

// TBX-R7: a chatty build (thousands of lines) used to render every line and
// smooth-scroll on every append, hijacking the page scroll and janking the
// renderer. Now we cap the rendered tail, scroll the container itself (not the
// page) instantly, and only auto-scroll when the user is pinned to the bottom.
const MAX_LINES = 500;

export default function LogStream({ lines, maxHeight = '200px' }: LogStreamProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const pinnedRef = useRef(true);

  const onScroll = () => {
    const el = containerRef.current;
    if (!el) return;
    pinnedRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  };

  useEffect(() => {
    const el = containerRef.current;
    if (el && pinnedRef.current) el.scrollTop = el.scrollHeight;
  }, [lines]);

  if (lines.length === 0) {
    return (
      <div
        className="select-text bg-black/40 border border-ue-border rounded p-3 text-gray-600 text-xs overflow-auto"
        style={{ maxHeight }}
      >
        (output will appear here)
      </div>
    );
  }

  const hidden = Math.max(0, lines.length - MAX_LINES);
  const visible = hidden ? lines.slice(-MAX_LINES) : lines;

  return (
    <div
      ref={containerRef}
      onScroll={onScroll}
      className="select-text bg-black/40 border border-ue-border rounded p-3 text-xs overflow-auto"
      style={{ maxHeight }}
    >
      {hidden > 0 && (
        <div className="text-gray-600 italic mb-1">({hidden.toLocaleString()} earlier line(s) hidden)</div>
      )}
      {visible.map((line, i) => {
        const isError = /\[error\]|error:|failed|exception/i.test(line);
        const isWarn = /warning:|warn:/i.test(line);
        const isOk = /done|success|complete|green|✓/i.test(line);
        const cls = isError ? 'text-red-400' : isWarn ? 'text-yellow-400' : isOk ? 'text-green-400' : 'text-gray-300';
        return (
          <div key={hidden + i} className={`whitespace-pre-wrap break-all ${cls}`}>{line}</div>
        );
      })}
    </div>
  );
}
