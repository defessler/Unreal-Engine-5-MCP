import type { Page } from '../App';

interface ModeSwitchProps {
  current: Page;
  onNav: (page: Page) => void;
}

// The two primary "modes" of the Toolbox. Install = first-time setup (mount the
// plugin, optionally build the server, configure a client). Update = upgrade an
// existing install. They used to be two of five flat sidebar items (Update at
// the very bottom), so the relationship — and that you can switch — wasn't
// obvious. This segmented switch surfaces it at the top of both pages.
const MODES: { id: Extract<Page, 'install' | 'update'>; label: string; sub: string }[] = [
  { id: 'install', label: 'Install', sub: 'First-time setup' },
  { id: 'update', label: 'Update', sub: 'Existing install' },
];

export default function ModeSwitch({ current, onNav }: ModeSwitchProps) {
  return (
    <div className="px-6 pt-5">
      <div
        role="tablist"
        aria-label="Setup mode"
        className="flex max-w-2xl gap-1 rounded-lg border border-ue-border bg-black/30 p-1"
      >
        {MODES.map((m) => {
          const active = current === m.id;
          return (
            <button
              key={m.id}
              role="tab"
              aria-selected={active}
              onClick={() => onNav(m.id)}
              title={`${m.label} — ${m.sub}`}
              className={`flex-1 rounded-md px-4 py-2.5 text-center transition-colors ${
                active
                  ? 'bg-ue-accent text-white shadow'
                  : 'text-gray-400 hover:bg-white/5 hover:text-gray-200'
              }`}
            >
              <div className="text-sm font-semibold leading-tight">{m.label}</div>
              <div className={`text-[11px] leading-tight ${active ? 'text-white/80' : 'text-gray-500'}`}>
                {m.sub}
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}
