import { useEffect, useRef } from 'react';

interface LogStreamProps {
  lines: string[];
  maxHeight?: string;
}

export default function LogStream({ lines, maxHeight = '200px' }: LogStreamProps) {
  const bottomRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [lines]);

  if (lines.length === 0) {
    return (
      <div
        className="bg-black/40 border border-ue-border rounded p-3 text-gray-600 text-xs overflow-auto"
        style={{ maxHeight }}
      >
        (output will appear here)
      </div>
    );
  }

  return (
    <div
      className="bg-black/40 border border-ue-border rounded p-3 text-xs overflow-auto"
      style={{ maxHeight }}
    >
      {lines.map((line, i) => {
        const isError = /\[error\]|error:|failed|exception/i.test(line);
        const isWarn = /warning:|warn:/i.test(line);
        const isOk = /done|success|complete|green|✓/i.test(line);
        const cls = isError ? 'text-red-400' : isWarn ? 'text-yellow-400' : isOk ? 'text-green-400' : 'text-gray-300';
        return (
          <div key={i} className={`whitespace-pre-wrap break-all ${cls}`}>{line}</div>
        );
      })}
      <div ref={bottomRef} />
    </div>
  );
}
