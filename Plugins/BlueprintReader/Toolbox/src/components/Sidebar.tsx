import type { Page } from '../App';

interface NavItem {
  id: Page;
  label: string;
  icon: string;
  description: string;
}

const NAV_ITEMS: NavItem[] = [
  { id: 'install', label: 'Install', icon: '⬇', description: 'Mount plugin & build server' },
  { id: 'providers', label: 'Providers', icon: '⚙', description: 'Configure AI clients' },
  { id: 'settings', label: 'Settings', icon: '🔧', description: 'Server flags & env vars' },
  { id: 'tester', label: 'Tester', icon: '▶', description: 'Test MCP tool calls' },
  { id: 'update', label: 'Update', icon: '↑', description: 'Check for updates' },
];

interface SidebarProps {
  current: Page;
  onNav: (page: Page) => void;
}

export default function Sidebar({ current, onNav }: SidebarProps) {
  return (
    <aside className="w-48 flex-shrink-0 bg-ue-panel border-r border-ue-border flex flex-col">
      <div className="px-4 py-4 border-b border-ue-border">
        <div className="text-xs text-ue-accent font-bold tracking-widest uppercase">BlueprintReader</div>
        <div className="text-xs text-gray-500 mt-0.5">Toolbox</div>
      </div>
      <nav className="flex-1 py-2">
        {NAV_ITEMS.map((item) => (
          <button
            key={item.id}
            onClick={() => onNav(item.id)}
            className={`w-full text-left px-4 py-3 flex items-start gap-2 hover:bg-white/5 transition-colors ${
              current === item.id ? 'bg-white/10 border-l-2 border-ue-accent' : 'border-l-2 border-transparent'
            }`}
          >
            <span className="text-base leading-none mt-0.5">{item.icon}</span>
            <div>
              <div className={`text-sm font-medium ${current === item.id ? 'text-white' : 'text-gray-300'}`}>
                {item.label}
              </div>
              <div className="text-xs text-gray-500 mt-0.5">{item.description}</div>
            </div>
          </button>
        ))}
      </nav>
      <div className="px-4 py-3 border-t border-ue-border">
        <div className="text-xs text-gray-600">v0.4.0</div>
      </div>
    </aside>
  );
}
