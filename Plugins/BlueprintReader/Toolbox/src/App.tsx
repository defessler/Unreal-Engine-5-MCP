import { useState } from 'react';
import Sidebar from './components/Sidebar';
import Install from './pages/Install';
import Providers from './pages/Providers';
import Settings from './pages/Settings';
import Tester from './pages/Tester';
import Update from './pages/Update';

export type Page = 'install' | 'providers' | 'settings' | 'tester' | 'update';

export default function App() {
  const [page, setPage] = useState<Page>('install');

  return (
    <div className="flex h-screen overflow-hidden">
      <Sidebar current={page} onNav={setPage} />
      <main className="flex-1 overflow-auto bg-ue-dark">
        {page === 'install' && <Install />}
        {page === 'providers' && <Providers />}
        {page === 'settings' && <Settings />}
        {page === 'tester' && <Tester />}
        {page === 'update' && <Update />}
      </main>
    </div>
  );
}
