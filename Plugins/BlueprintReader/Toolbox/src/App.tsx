import { useState } from 'react';
import Sidebar from './components/Sidebar';
import ModeSwitch from './components/ModeSwitch';
import ErrorBoundary from './components/ErrorBoundary';
import Install from './pages/Install';
import Providers from './pages/Providers';
import Settings from './pages/Settings';
import Tester from './pages/Tester';
import Update from './pages/Update';
import { bridge } from './lib/bridge';

export type Page = 'install' | 'providers' | 'settings' | 'tester' | 'update';

function TitleBar() {
  return (
    <div
      className="flex items-center h-8 flex-shrink-0 bg-ue-title border-b border-black/50 select-none"
      style={{ WebkitAppRegion: 'drag' } as React.CSSProperties}
    >
      <div className="flex items-center gap-2 px-3">
        <svg width="14" height="14" viewBox="0 0 16 16" fill="none" className="flex-shrink-0">
          <rect x="1" y="1" width="14" height="14" rx="2" fill="#E87722" opacity="0.9"/>
          <path d="M4 8 L8 4 L12 8 L8 12 Z" fill="white" opacity="0.9"/>
        </svg>
        <span className="text-xs text-gray-400 font-medium tracking-wide">BlueprintReader Toolbox</span>
      </div>
      <div
        className="ml-auto flex"
        style={{ WebkitAppRegion: 'no-drag' } as React.CSSProperties}
      >
        <button
          onClick={() => bridge.minimizeWindow()}
          aria-label="Minimize window" title="Minimize"
          className="w-11 h-8 flex items-center justify-center text-gray-500 hover:text-gray-200 hover:bg-white/10 transition-colors"
        >
          <svg width="10" height="2" viewBox="0 0 10 2" fill="currentColor"><rect width="10" height="1.5" rx="0.75"/></svg>
        </button>
        <button
          onClick={() => bridge.maximizeWindow()}
          aria-label="Maximize window" title="Maximize"
          className="w-11 h-8 flex items-center justify-center text-gray-500 hover:text-gray-200 hover:bg-white/10 transition-colors"
        >
          <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2"><rect x="0.6" y="0.6" width="8.8" height="8.8" rx="0.5"/></svg>
        </button>
        <button
          onClick={() => bridge.closeWindow()}
          aria-label="Close window" title="Close"
          className="w-11 h-8 flex items-center justify-center text-gray-500 hover:text-white hover:bg-red-600 transition-colors"
        >
          <svg width="10" height="10" viewBox="0 0 10 10" stroke="currentColor" strokeWidth="1.4"><line x1="1" y1="1" x2="9" y2="9"/><line x1="9" y1="1" x2="1" y2="9"/></svg>
        </button>
      </div>
    </div>
  );
}

export default function App() {
  // Always launch on the Install tab — the natural starting point (first-time
  // setup). The page is intentionally NOT persisted across launches, so every
  // open lands here regardless of where the last session ended.
  const [page, setPage] = useState<Page>('install');

  return (
    <div className="flex flex-col h-screen overflow-hidden">
      <TitleBar />
      <div className="flex flex-1 overflow-hidden">
        <Sidebar current={page} onNav={setPage} />
        <main className="flex-1 overflow-auto bg-ue-dark">
          {/* Prominent Install↔Update mode switch, shown on both those pages so
              the current mode — and that you can flip — is always obvious. */}
          {(page === 'install' || page === 'update') && (
            <ModeSwitch current={page} onNav={setPage} />
          )}
          {/* Remount the boundary per page so dismissing an error on one page
              doesn't suppress a real error on the next (P1). */}
          <ErrorBoundary key={page}>
            {page === 'install' && <Install />}
            {page === 'providers' && <Providers />}
            {page === 'settings' && <Settings />}
            {page === 'tester' && <Tester />}
            {page === 'update' && <Update />}
          </ErrorBoundary>
        </main>
      </div>
    </div>
  );
}
