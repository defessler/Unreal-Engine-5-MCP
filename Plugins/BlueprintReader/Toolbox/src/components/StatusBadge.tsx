export type Status = 'configured' | 'stale' | 'missing' | 'loading';

interface StatusBadgeProps {
  status: Status;
  label?: string;
}

const CONFIG: Record<Status, { color: string; icon: string; text: string }> = {
  configured: { color: 'text-green-400 bg-green-400/10', icon: '✓', text: 'Configured' },
  stale:      { color: 'text-yellow-400 bg-yellow-400/10', icon: '⚠', text: 'Stale' },
  missing:    { color: 'text-red-400 bg-red-400/10', icon: '✗', text: 'Missing' },
  loading:    { color: 'text-gray-400 bg-gray-400/10', icon: '…', text: 'Checking' },
};

export default function StatusBadge({ status, label }: StatusBadgeProps) {
  const c = CONFIG[status];
  return (
    <span className={`inline-flex items-center gap-1 px-2 py-0.5 rounded text-xs font-medium ${c.color}`}>
      <span>{c.icon}</span>
      <span>{label ?? c.text}</span>
    </span>
  );
}
