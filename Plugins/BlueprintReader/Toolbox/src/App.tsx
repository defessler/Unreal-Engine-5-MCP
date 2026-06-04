import { useState } from 'react';
import Sidebar from './components/Sidebar';
import Install from './pages/Install';
import Providers from './pages/Providers';
import Tester from './pages/Tester';
import Update from './pages/Update';

export type Page = 'install' | 'providers' | 'tester' | 'update';

export default function App() {
  const [page, setPage] = useState<Page>('install');

  return (
    <div className="flex h-screen overflow-hidden">
      <Sidebar current={page} onNav={setPage} />
      <main className="flex-1 overflow-auto bg-ue-dark">
        {page === 'install' && <Install />}
        {page === 'providers' && <Providers />}
        {page === 'tester' && <Tester />}
        {page === 'update' && <Update />}
      </main>
    </div>
  );
}
