import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { copyFileSync, mkdirSync } from 'fs';
import { resolve } from 'path';

export default defineConfig({
  plugins: [
    react(),
    {
      name: 'copy-tools-json',
      buildStart() {
        // __dirname is available because Vite runs this config as CJS (no "type":"module").
        const src = resolve(__dirname, '../../../docs/tools.json');
        const destDir = resolve(__dirname, 'src/assets');
        mkdirSync(destDir, { recursive: true });
        copyFileSync(src, resolve(destDir, 'tools.json'));
      },
    },
  ],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
  },
});
