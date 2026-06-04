// Copies docs/tools.json into src/assets/ before the Vite build.
// Run via: npm run prebuild:assets
const { copyFileSync, mkdirSync } = require('fs');
const { resolve } = require('path');

const src = resolve(__dirname, '../../../../docs/tools.json');
const destDir = resolve(__dirname, '../src/assets');
mkdirSync(destDir, { recursive: true });
copyFileSync(src, resolve(destDir, 'tools.json'));
console.log('Copied docs/tools.json -> src/assets/tools.json');
