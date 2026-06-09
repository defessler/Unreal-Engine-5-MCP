import {
  app,
  BrowserWindow,
  ipcMain,
  dialog,
  shell,
  Menu,
  session,
} from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { pipeline } from 'node:stream/promises';
import { Readable, Transform } from 'node:stream';
import { spawn, execFileSync, ChildProcess } from 'child_process';
import { fileURLToPath } from 'node:url';

const isDev = !app.isPackaged;

// ---------------------------------------------------------------------------
// Child-process tracking (TBX-R1)
// ---------------------------------------------------------------------------
// Every transient child we spawn (installs, extraction, scripts, kill sweeps) is
// registered here so the window-close handler can tear down the whole tree —
// orphaned PowerShell/build processes otherwise keep locks on the very plugin
// dir a later op tries to swap. A `cancel-operation` IPC kills them on demand.
const transientChildren = new Set<ChildProcess>();

function trackChild<T extends ChildProcess>(p: T): T {
  transientChildren.add(p);
  p.once('exit', () => transientChildren.delete(p));
  return p;
}

// taskkill /T kills the whole process tree (a build spawns UBT/cl/link children
// that node's proc.kill() would orphan); /F forces it. Best-effort + detached.
function killTree(pid: number | undefined): void {
  if (!pid) return;
  try {
    spawn('taskkill', ['/PID', String(pid), '/T', '/F'],
      { windowsHide: true, stdio: 'ignore', detached: true }).unref();
  } catch { /* ignore */ }
}

function killAllTransient(): void {
  for (const p of transientChildren) killTree(p.pid);
  transientChildren.clear();
}

// In-flight download fetches — so cancel-operation can abort the download phase
// (the longest part of an install/update), not just child processes (R1/M1).
const activeDownloads = new Set<AbortController>();

// Monotonic suffix so two same-path atomic writes can't collide on the temp name.
let writeSeq = 0;

// ---------------------------------------------------------------------------
// Security helpers (Batch 1a hardening)
// ---------------------------------------------------------------------------

// Resolve the realpath of `p`'s deepest EXISTING ancestor, then re-append the
// non-existent tail — so any symlink/junction in the path is followed BEFORE
// the under-root test. Without this, isPathUnder is a pure-string check that a
// junction planted inside an allowed root would defeat (it would validate the
// spelled path, not the real target).
function realResolve(p: string): string {
  let cur = path.resolve(p);
  const tail: string[] = [];
  for (let i = 0; i < 64 && !fs.existsSync(cur); i++) {
    const parent = path.dirname(cur);
    if (parent === cur) break;
    tail.unshift(path.basename(cur));
    cur = parent;
  }
  try { cur = fs.realpathSync.native(cur); } catch { /* keep resolved */ }
  return tail.length ? path.join(cur, ...tail) : cur;
}

// True if `child`'s real target is inside one of `parents` (prevents `..`
// traversal AND symlink/junction escape out of an allowed root).
function isPathUnder(child: string, parents: string[]): boolean {
  const resolved = realResolve(child);
  if (resolved.includes('\0')) return false;
  return parents.some((parent) => {
    let root: string;
    try { root = fs.realpathSync.native(path.resolve(parent)); }
    catch { root = path.resolve(parent); }
    const rel = path.relative(root, resolved);
    return rel === '' || (!rel.startsWith('..') && !path.isAbsolute(rel));
  });
}

// Roots the renderer is allowed to read/write through the file IPCs: the user's
// home (client MCP configs live in ~/.claude.json, ~/.cursor, ~/.codeium, …),
// the configured project tree, and the OS temp dir. Blocks reads/writes of
// arbitrary system files (e.g. C:\Windows\…, registry-run drop targets).
function allowedFileRoots(): string[] {
  const roots = [app.getPath('home'), app.getPath('appData'), app.getPath('temp')];
  const proj = getProjectDir();
  if (proj) roots.push(proj);
  return roots.filter(Boolean);
}

// Secret / persistence subpaths under home that are NEVER legit MCP-config
// targets — denied even though they fall under the allowed home root (the broad
// home grant is needed for the dotfile configs, so deny the known-sensitive
// pockets explicitly).
function deniedRoots(): string[] {
  const home = app.getPath('home');
  return [
    path.join(home, '.ssh'), path.join(home, '.aws'), path.join(home, '.gnupg'),
    path.join(home, '.config', 'gh'),
    path.join(app.getPath('appData'), 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'Startup'),
  ];
}

// A path the file IPCs may touch: under an allowed root AND not under a denied one.
function fileAccessAllowed(p: string): boolean {
  return isPathUnder(p, allowedFileRoots()) && !isPathUnder(p, deniedRoots());
}

// ---------------------------------------------------------------------------
// Path inference
// ---------------------------------------------------------------------------

// Persistent project path stored in userData so the portable exe remembers
// the project across launches. Written by the 'save-project' IPC handler
// when the user picks a .uproject on the Install page.
function savedProjectFilePath(): string {
  return path.join(app.getPath('userData'), 'project.json');
}
function loadSavedProject(): string {
  try {
    const raw = fs.readFileSync(savedProjectFilePath(), 'utf8');
    const obj = JSON.parse(raw) as { uproject?: string };
    const up = obj.uproject ?? '';
    if (up && fs.existsSync(up)) return path.dirname(up);
  } catch { /* not saved yet */ }
  return '';
}
function saveProject(uprojectPath: string): void {
  try {
    fs.mkdirSync(app.getPath('userData'), { recursive: true });
    fs.writeFileSync(savedProjectFilePath(), JSON.stringify({ uproject: uprojectPath }), 'utf8');
  } catch { /* ignore write errors */ }
}

function getProjectDir(): string {
  // 1. CLI arg --project-dir=<path>
  const argPrefix = '--project-dir=';
  for (const arg of process.argv) {
    if (arg.startsWith(argPrefix)) {
      const p = arg.slice(argPrefix.length);
      if (fs.existsSync(p)) return p;
    }
  }
  // 2. Env var
  if (process.env['TOOLBOX_PROJECT_DIR'] && fs.existsSync(process.env['TOOLBOX_PROJECT_DIR'])) {
    return process.env['TOOLBOX_PROJECT_DIR'];
  }
  // 3. Persisted project (user selected on Install page)
  const saved = loadSavedProject();
  if (saved) return saved;
  // 4. Walk up from __dirname (works in dev: Toolbox/ -> BlueprintReader/ -> Plugins/ -> project)
  let dir = __dirname;
  for (let i = 0; i < 6; i++) {
    try {
      const files = fs.readdirSync(dir).filter(f => f.endsWith('.uproject'));
      if (files.length > 0) return dir;
    } catch { /* skip unreadable dirs */ }
    dir = path.dirname(dir);
  }
  return '';
}

function findUproject(projectDir: string): string {
  const files = fs.existsSync(projectDir)
    ? fs.readdirSync(projectDir).filter(f => f.endsWith('.uproject'))
    : [];
  return files.length > 0 ? path.join(projectDir, files[0]) : '';
}

function getPluginDir(projectDir: string): string {
  return path.join(projectDir, 'Plugins', 'BlueprintReader');
}

function getExePath(projectDir: string): string {
  const pluginBin = path.join(projectDir, 'Plugins', 'BlueprintReader', 'Binaries', 'Win64', 'BlueprintReaderMcp.exe');
  if (fs.existsSync(pluginBin)) return pluginBin;
  const legacyBin = path.join(projectDir, 'Binaries', 'Win64', 'BlueprintReaderMcp.exe');
  if (fs.existsSync(legacyBin)) return legacyBin;
  return pluginBin; // return the expected path even if not built yet
}

function regQuery(key: string, name: string): string | null {
  try {
    // execFile (no shell): `reg` receives key/name as literal argv elements, so
    // a crafted EngineAssociation in a .uproject (e.g. `" & calc & "`) can't
    // break out and run commands. stderr is swallowed via stdio:'pipe' + catch.
    const out = execFileSync('reg', ['query', key, '/v', name], {
      encoding: 'utf8', timeout: 3000, windowsHide: true, stdio: ['ignore', 'pipe', 'ignore'],
    });
    const match = out.match(/REG_SZ\s+(.+)/);
    return match ? match[1].trim() : null;
  } catch {
    return null;
  }
}

function getEngineDir(uprojectFile: string): string {
  if (process.env['BP_READER_ENGINE_DIR'] && fs.existsSync(process.env['BP_READER_ENGINE_DIR'])) {
    return process.env['BP_READER_ENGINE_DIR'];
  }

  let assoc: string | null = null;
  if (uprojectFile && fs.existsSync(uprojectFile)) {
    try {
      const proj = JSON.parse(fs.readFileSync(uprojectFile, 'utf8'));
      assoc = proj.EngineAssociation ?? null;
    } catch {
      // ignore
    }
  }

  if (assoc) {
    // GUID -> HKCU source build
    if (/^\{?[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\}?$/.test(assoc)) {
      const bare = assoc.replace(/[{}]/g, '');
      for (const variant of [assoc, `{${bare}}`, bare]) {
        const p = regQuery(`HKCU\\Software\\Epic Games\\Unreal Engine\\Builds`, variant);
        if (p && fs.existsSync(p)) return p;
      }
    } else if (/^[0-9]+\.[0-9]+(\.[0-9]+)?$/.test(assoc)) {
      // Version string (e.g. "5.8") -> HKLM. Strict pattern so a junk/hostile
      // EngineAssociation can't be used to build a registry path or `UE_<x>`
      // scan dir (belt-and-suspenders alongside the execFile change).
      for (const base of [
        `HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\${assoc}`,
        `HKLM\\SOFTWARE\\WOW6432Node\\EpicGames\\Unreal Engine\\${assoc}`,
      ]) {
        const p = regQuery(base, 'InstalledDirectory');
        if (p && fs.existsSync(p)) return p;
      }
      // Scan common dirs
      for (const root of ['C:\\Program Files\\Epic Games', 'D:\\Epic Games', 'D:\\Games\\Epic Games']) {
        const cand = path.join(root, `UE_${assoc}`);
        if (fs.existsSync(path.join(cand, 'Engine'))) return cand;
      }
    }
  }

  // Fallback: scan common Launcher dirs for any UE install
  for (const root of ['C:\\Program Files\\Epic Games', 'D:\\Epic Games', 'D:\\Games\\Epic Games']) {
    try {
      const dirs = fs.readdirSync(root).filter(d => d.startsWith('UE_'));
      if (dirs.length > 0) {
        const cand = path.join(root, dirs[0]);
        if (fs.existsSync(path.join(cand, 'Engine'))) return cand;
      }
    } catch {
      // dir doesn't exist
    }
  }
  return '';
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

let mainWindow: BrowserWindow | null = null;
let splash: BrowserWindow | null = null;

// Tiny frameless splash shown instantly while Chromium + the renderer warm up,
// so the user sees branding immediately instead of waiting on a blank screen.
function createSplash() {
  splash = new BrowserWindow({
    width: 380, height: 220,
    frame: false, transparent: false, resizable: false, movable: false,
    alwaysOnTop: true, skipTaskbar: true, show: true,
    backgroundColor: '#1a1a1a',
    webPreferences: { contextIsolation: true, nodeIntegration: false },
  });
  const html = `<!doctype html><meta charset="utf-8"><style>
    html,body{margin:0;height:100%;background:#1a1a1a;color:#e5e5e5;
      font-family:-apple-system,Segoe UI,Roboto,sans-serif;overflow:hidden;user-select:none}
    .wrap{display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;gap:14px}
    .logo{width:44px;height:44px}
    .title{font-size:14px;letter-spacing:.04em;color:#bdbdbd}
    .sub{font-size:11px;color:#7a7a7a}
    .bar{width:160px;height:3px;border-radius:3px;background:#2d2d2d;overflow:hidden}
    .bar>i{display:block;height:100%;width:40%;background:#E87722;border-radius:3px;animation:slide 1.1s ease-in-out infinite}
    @keyframes slide{0%{margin-left:-40%}100%{margin-left:100%}}
  </style><div class="wrap">
    <svg class="logo" viewBox="0 0 16 16"><rect x="1" y="1" width="14" height="14" rx="3" fill="#E87722"/><path d="M4 8 L8 4 L12 8 L8 12 Z" fill="#fff"/></svg>
    <div class="title">BlueprintReader Toolbox</div>
    <div class="bar"><i></i></div>
    <div class="sub">Starting up…</div>
  </div>`;
  splash.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
  splash.on('closed', () => { splash = null; });
}

function createWindow() {
  // Remove the native File/Edit/View/Window/Help menu bar entirely.
  Menu.setApplicationMenu(null);
  createSplash();

  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: 900,
    minHeight: 600,
    backgroundColor: '#1a1a1a',
    frame: false,          // custom title bar in renderer
    show: false,           // reveal on ready-to-show to avoid a blank-window flash
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
    title: 'BlueprintReader Toolbox',
  });

  // Navigation lockdown (TBX-S5): deny window.open / target=_blank (route
  // http(s) to the OS browser instead), and block any attempt to navigate the
  // privileged renderer away from its own origin — a navigated-away page would
  // retain the preload bridge. SPA routing uses the history API (no navigation
  // event), so this never fires for normal in-app routing.
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//i.test(url)) void shell.openExternal(url);
    return { action: 'deny' };
  });
  const guardNavigation = (e: Electron.Event, url: string) => {
    const ok = isDev ? url.startsWith('http://localhost:5173') : url.startsWith('file://');
    if (!ok) e.preventDefault();
  };
  mainWindow.webContents.on('will-navigate', guardNavigation);
  mainWindow.webContents.on('will-redirect', guardNavigation);

  // Show the real window only once the renderer has painted its first frame,
  // then dismiss the splash. Cuts perceived startup to "splash → app".
  mainWindow.once('ready-to-show', () => {
    mainWindow?.show();
    if (splash && !splash.isDestroyed()) splash.close();
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools();
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
    if (splash && !splash.isDestroyed()) { splash.close(); }
    // Clean up any spawned servers + tear down every transient child tree (R1)
    // so an in-flight install/build can't orphan PowerShell/UBT/cl processes
    // that keep locks on the plugin dir.
    for (const [, proc] of serverProcesses) {
      try { proc.kill(); } catch { /* ignore */ }
    }
    killAllTransient();
  });
}

// Reap stale temp dirs from prior installs / self-updates. The self-update path
// intentionally can't delete its own dir before quitting (the detached helper
// still needs it), and a killed install could orphan one too. A dir a fresh
// helper still holds is simply skipped and retried next launch.
// TBX-R6: async + run off the launch critical path (a recursive rm of a large
// stale dir on a slow drive otherwise delayed the first window).
async function sweepStaleTempDirs() {
  try {
    const tmpRoot = app.getPath('temp');
    const names = await fs.promises.readdir(tmpRoot);
    for (const name of names) {
      if (/^bpr-(tbup|dl|assets)-/.test(name)) {
        try { await fs.promises.rm(path.join(tmpRoot, name), { recursive: true, force: true }); } catch { /* in use; next time */ }
      }
    }
  } catch { /* ignore */ }
}

// Content-Security-Policy for the packaged app (defense-in-depth alongside
// contextIsolation). Skipped in dev — Vite's HMR needs ws: + inline/eval. The
// renderer only talks to 'self' (the bundle) and the local MCP server over HTTP.
function installCsp() {
  if (isDev) return;
  session.defaultSession.webRequest.onHeadersReceived((details, cb) => {
    cb({ responseHeaders: {
      ...details.responseHeaders,
      'Content-Security-Policy': [
        "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; " +
        "script-src 'self'; connect-src 'self' http://127.0.0.1:* http://localhost:*",
      ],
    } });
  });
}

app.whenReady().then(() => { installCsp(); createWindow(); void sweepStaleTempDirs(); });
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });

// ---------------------------------------------------------------------------
// IPC handlers
// ---------------------------------------------------------------------------

ipcMain.handle('get-app-version', () => app.getVersion());
ipcMain.handle('get-env-paths', () => ({
  userProfile: process.env['USERPROFILE'] ?? app.getPath('home'),
  appData:     process.env['APPDATA']     ?? app.getPath('appData'),
}));

// Window controls for the custom (frameless) title bar
ipcMain.handle('minimize-window', () => mainWindow?.minimize());
ipcMain.handle('maximize-window', () => {
  if (mainWindow?.isMaximized()) mainWindow.restore();
  else mainWindow?.maximize();
});
ipcMain.handle('close-window', () => mainWindow?.close());

// Engine dir resolution from a .uproject path — used by Install page to
// show the auto-detected engine without requiring a separate engine field.
ipcMain.handle('resolve-engine', (_evt, uprojectPath: string) => {
  return getEngineDir(uprojectPath);
});

// Persist the user-chosen .uproject so all pages can resolve pluginDir
// correctly even when the portable exe is run outside the project tree.
ipcMain.handle('save-project', (_evt, uprojectPath: string) => {
  // Only persist a real .uproject — stops the renderer relocating the project
  // root (which feeds the run-script / file allowlists) to an arbitrary dir.
  if (typeof uprojectPath === 'string'
      && uprojectPath.toLowerCase().endsWith('.uproject')
      && fs.existsSync(uprojectPath)) {
    saveProject(uprojectPath);
  }
});

// A read-only existence boolean for a .uproject — leaks nothing, so it's exempt
// from the read-file allowlist. The Install page validates the picked project
// with this BEFORE the project root is persisted (so it works for a first pick
// on a drive the allowlist doesn't yet include).
ipcMain.handle('uproject-exists', (_evt, p: string) => {
  return typeof p === 'string' && p.toLowerCase().endsWith('.uproject') && fs.existsSync(p);
});

ipcMain.handle('get-paths', () => {
  const projectDir = getProjectDir();
  // When running as a portable exe outside any project, projectDir may be
  // empty until the user picks a .uproject on the Install page. Return safe
  // empty strings rather than a garbage temp-dir path.
  const uproject = projectDir ? findUproject(projectDir) : '';
  return {
    projectDir,
    pluginDir: projectDir ? getPluginDir(projectDir) : '',
    exePath: projectDir ? getExePath(projectDir) : '',
    engineDir: uproject ? getEngineDir(uproject) : '',
    uproject,
  };
});

// TBX-R6: async fs so a slow drive can't freeze all IPC / the window.
ipcMain.handle('read-file', async (_evt, filePath: string) => {
  if (!fileAccessAllowed(filePath)) return null;
  try {
    return await fs.promises.readFile(filePath, 'utf8');
  } catch {
    return null;
  }
});

ipcMain.handle('write-file', async (_evt, filePath: string, content: string) => {
  if (!fileAccessAllowed(filePath)) {
    throw new Error(`refused to write outside allowed roots: ${filePath}`);
  }
  await fs.promises.mkdir(path.dirname(filePath), { recursive: true });
  // Atomic write: a crash/lock mid-write can't leave a half-written config —
  // write a sibling temp, then rename over the target (libuv rename replaces).
  const tmp = `${filePath}.tmp-${process.pid}-${writeSeq++}`;
  try {
    await fs.promises.writeFile(tmp, content, 'utf8');
    await fs.promises.rename(tmp, filePath);
  } catch (e) {
    await fs.promises.rm(tmp, { force: true }).catch(() => {});
    throw e;
  }
});

ipcMain.handle('open-file-dialog', async (_evt, opts: {
  title?: string;
  defaultPath?: string;
  filters?: { name: string; extensions: string[] }[];
  properties?: Array<'openFile' | 'openDirectory'>;
}) => {
  if (!mainWindow) return null;
  const result = await dialog.showOpenDialog(mainWindow, {
    title: opts.title,
    defaultPath: opts.defaultPath,
    filters: opts.filters,
    properties: opts.properties ?? ['openFile'],
  });
  return result.canceled ? null : result.filePaths[0];
});

ipcMain.handle('open-external', async (_evt, url: string) => {
  // Only ever hand http(s) URLs to the OS browser — never smb:/javascript:/etc.,
  // which shell.openExternal would otherwise launch (local-exe / SMB-creds risk).
  // The 'Open config file' button passes a file:// URL: open it via shell.openPath
  // (not openExternal), and only if it's a path the app is already allowed to read.
  try {
    const u = new URL(url);
    if (u.protocol === 'https:' || u.protocol === 'http:') {
      await shell.openExternal(url);
    } else if (u.protocol === 'file:') {
      const p = fileURLToPath(url);
      if (fileAccessAllowed(p)) await shell.openPath(p);
    }
  } catch { /* malformed URL — ignore */ }
});

// ---------------------------------------------------------------------------
// Self-contained download / install / self-update (GitHub Releases)
// ---------------------------------------------------------------------------
// Makes the Toolbox all-encompassing: it can download the plugin from GitHub
// Releases and install/update it into a project (no pre-existing in-project
// script needed), and update its own portable exe — see the *-from-release and
// self-update-toolbox IPCs below. Progress + logs reuse the 'script-log' channel
// so the renderer's LogStream shows them with no extra wiring.

const GH_REPO = 'defessler/Unreal-Engine-5-MCP';

interface ReleaseAsset { name: string; url: string; size: number; digest?: string; }
interface ReleaseInfo { tag: string; assets: ReleaseAsset[]; }

function logLine(msg: string) {
  mainWindow?.webContents.send('script-log', msg.endsWith('\n') ? msg : msg + '\n');
}

// Semver-aware compare: >0 if a newer than b, <0 older, 0 equal. Compares the
// numeric core, THEN pre-release (a version WITH a pre-release ranks BELOW the
// same core without one — so v0.6.0 > v0.6.0-rc2); build metadata (+…) is
// ignored. The old version stripped the whole `-suffix`, making v0.6.0-rc2 ==
// v0.6.0 and hiding real updates / risking a wrong downgrade (TBX-F8).
function cmpVersion(a: string, b: string): number {
  const split = (s: string) => {
    const clean = (s ?? '').trim().replace(/^v/i, '').split('+')[0];  // drop build meta
    const [core, pre = ''] = clean.split('-');
    return { core: core.split('.').map(n => parseInt(n, 10) || 0), pre };
  };
  const A = split(a), B = split(b);
  for (let i = 0; i < Math.max(A.core.length, B.core.length); i++) {
    const d = (A.core[i] ?? 0) - (B.core[i] ?? 0);
    if (d !== 0) return d > 0 ? 1 : -1;
  }
  if (!A.pre && !B.pre) return 0;
  if (!A.pre) return 1;   // a is a release, b is a pre-release of the same core
  if (!B.pre) return -1;
  const pa = A.pre.split('.'), pb = B.pre.split('.');
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i], y = pb[i];
    if (x === undefined) return -1;   // shorter pre-release set ranks lower
    if (y === undefined) return 1;
    const nx = /^\d+$/.test(x), ny = /^\d+$/.test(y);
    if (nx && ny) { const d = parseInt(x, 10) - parseInt(y, 10); if (d) return d > 0 ? 1 : -1; }
    else if (nx) return -1;           // numeric identifiers rank below alphanumeric
    else if (ny) return 1;
    else if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}

async function fetchLatestRelease(): Promise<ReleaseInfo> {
  const res = await fetch(`https://api.github.com/repos/${GH_REPO}/releases/latest`, {
    headers: { 'User-Agent': 'bp-reader-toolbox', 'Accept': 'application/vnd.github+json' },
    signal: AbortSignal.timeout(30000),
  });
  if (!res.ok) throw new Error(`GitHub API returned HTTP ${res.status} (rate-limited or offline?)`);
  const j = await res.json() as { tag_name: string; assets?: Array<{ name: string; browser_download_url: string; size: number; digest?: string }> };
  return {
    tag: j.tag_name,
    assets: (j.assets ?? []).map(a => ({ name: a.name, url: a.browser_download_url, size: a.size, digest: a.digest })),
  };
}

// Stream a URL to disk with: a stall watchdog (aborts if no data for 60s, so a
// half-open socket can't wedge the UI forever), completeness check (got === size),
// and optional sha256 verification from the release asset's `digest`. Uses
// stream pipeline so a write error (disk full, AV lock, perms) rejects cleanly —
// the caller's try/catch turns it into {ok:false,error} instead of crashing the
// main process — and partial files are removed so a retry can't reuse them.
class DownloadError extends Error {
  constructor(message: string, readonly retriable: boolean, readonly retryAfterMs?: number) {
    super(message);
  }
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

// One download attempt → writes to `partPath` (a temp sibling), verifies size +
// optional sha256. Throws a DownloadError tagged retriable for transient faults.
async function downloadOnce(
  url: string, partPath: string, label: string,
  expected?: { size?: number; digest?: string },
): Promise<void> {
  const ctrl = new AbortController();
  activeDownloads.add(ctrl);
  let stall: NodeJS.Timeout | undefined;
  const STALL_MS = 60000;
  let stalled = false;
  const arm = () => { if (stall) clearTimeout(stall); stall = setTimeout(() => { stalled = true; ctrl.abort(); }, STALL_MS); };
  arm();
  let got = 0, lastPct = -1;
  try {
    const res = await fetch(url, { headers: { 'User-Agent': 'bp-reader-toolbox' }, redirect: 'follow', signal: ctrl.signal });
    if (!res.ok || !res.body) {
      // 5xx / 408 / 429 are transient; honor Retry-After when present.
      const retriable = res.status >= 500 || res.status === 408 || res.status === 429;
      const ra = Number(res.headers.get('retry-after'));
      throw new DownloadError(`download of ${label} failed: HTTP ${res.status}`, retriable, Number.isFinite(ra) ? ra * 1000 : undefined);
    }
    const total = Number(res.headers.get('content-length') ?? 0);
    const hash = expected?.digest ? crypto.createHash('sha256') : null;
    const progress = new Transform({
      transform(chunk: Buffer, _enc, cb) {
        arm();
        hash?.update(chunk);
        got += chunk.length;
        if (total) { const pct = Math.floor((got / total) * 100); if (pct >= lastPct + 10) { lastPct = pct; logLine(`  ${label}: ${pct}%`); } }
        cb(null, chunk);
      },
    });
    // Readable.fromWeb converts the WHATWG body; pipeline wires error handling +
    // destroys all streams (releases the fd) on any failure.
    await pipeline(Readable.fromWeb(res.body as Parameters<typeof Readable.fromWeb>[0]), progress, fs.createWriteStream(partPath));

    const wantSize = expected?.size || total;
    if (wantSize && got !== wantSize) {
      // A short read is a dropped connection — retriable.
      throw new DownloadError(`${label} download incomplete: ${got}/${wantSize} bytes (connection dropped?)`, true);
    }
    if (hash && expected?.digest) {
      const want = expected.digest.replace(/^sha256:/i, '').toLowerCase();
      if (hash.digest('hex') !== want) {
        // A complete-but-wrong file won't fix on retry — fail hard.
        throw new DownloadError(`${label} integrity check failed (sha256 mismatch — corrupt download)`, false);
      }
    }
    logLine(`  ${label}: downloaded ${(got / 1048576).toFixed(1)} MB`);
  } catch (e) {
    if (stalled) throw new DownloadError(`${label} download stalled (no data for ${STALL_MS / 1000}s)`, true);
    throw e;
  } finally {
    if (stall) clearTimeout(stall);
    activeDownloads.delete(ctrl);
  }
}

// Atomic, retrying download: each attempt streams to `${destPath}.part`; only a
// fully verified file is renamed into place, so a partial never masquerades as
// a finished download (R2). Leftover `.part` files are always removed. Transient
// failures (5xx/429/stall/short-read) get bounded backoff retries.
async function downloadFile(
  url: string, destPath: string, label: string,
  expected?: { size?: number; digest?: string },
): Promise<void> {
  const partPath = `${destPath}.part`;
  const MAX_ATTEMPTS = 3;
  let lastErr: unknown;
  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    try {
      await downloadOnce(url, partPath, label, expected);
      try { fs.rmSync(destPath, { force: true }); } catch { /* ignore */ }
      fs.renameSync(partPath, destPath);  // atomic publish
      return;
    } catch (e) {
      lastErr = e;
      try { fs.rmSync(partPath, { force: true }); } catch { /* ignore */ }
      const retriable = e instanceof DownloadError ? e.retriable : true; // network/abort errors aren't DownloadError
      if (!retriable || attempt >= MAX_ATTEMPTS) break;
      const backoff = (e instanceof DownloadError && e.retryAfterMs) ? e.retryAfterMs : 1000 * 2 ** (attempt - 1);
      logLine(`  ${label}: attempt ${attempt} failed — retrying in ${Math.round(backoff / 1000)}s ...`);
      await sleep(backoff);
    }
  }
  throw lastErr;
}

function runPwsh(args: string[]): Promise<number> {
  return new Promise((resolve) => {
    const p = trackChild(spawn('pwsh.exe', args, { windowsHide: true }));
    p.stdout.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.stderr.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.on('exit', (c) => resolve(c ?? 0));
    p.on('error', (e) => { logLine(`[error] ${e.message}`); resolve(1); });
  });
}

// Expand a .zip with a STATIC PowerShell command — the zip/dest paths are passed
// via environment variables, NOT interpolated into a `-Command` string, so a
// path containing PowerShell metacharacters can't inject. (Windows PowerShell
// 5.1, always present; `powershell.exe` not `pwsh.exe`.)
function expandArchive(zip: string, dest: string): Promise<number> {
  return new Promise((resolve) => {
    const p = trackChild(spawn('powershell.exe', ['-NoProfile', '-Command',
      'Expand-Archive -LiteralPath $env:BPR_ZIP -DestinationPath $env:BPR_DEST -Force'],
      { windowsHide: true, env: { ...process.env, BPR_ZIP: zip, BPR_DEST: dest } }));
    p.stdout.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.stderr.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.on('exit', (c) => resolve(c ?? 0));
    p.on('error', (e) => { logLine(`[error] ${e.message}`); resolve(1); });
  });
}

// The MCP client ids Install-Plugin.ps1's -Client ValidateSet accepts — and
// exactly what the Install page's <select> sends (PascalCase). Anything else is
// rejected before it can become a script argument. (Must match Install.tsx +
// the script's ValidateSet, or per-client install silently breaks.)
const ALLOWED_CLIENTS = new Set([
  'All', 'ClaudeCode', 'Cursor', 'VSCode', 'Rider', 'Gemini', 'Codex',
]);

// Find the dir containing BlueprintReader.uplugin inside an extracted archive.
function findPluginRoot(dir: string): string | null {
  const stack = [dir];
  while (stack.length) {
    const d = stack.pop()!;
    let entries: fs.Dirent[] = [];
    try { entries = fs.readdirSync(d, { withFileTypes: true }); } catch { continue; }
    if (entries.some(e => e.isFile() && e.name === 'BlueprintReader.uplugin')) return d;
    for (const e of entries) if (e.isDirectory()) stack.push(path.join(d, e.name));
  }
  return null;
}

ipcMain.handle('get-latest-release', async () => {
  try {
    const rel = await fetchLatestRelease();
    return {
      ok: true,
      tag: rel.tag,
      hasPlugin: rel.assets.some(a => /-plugin\.zip$/i.test(a.name)),
      hasToolbox: rel.assets.some(a => /-toolbox-win64\.exe$/i.test(a.name)),
    };
  } catch (e) {
    return { ok: false, error: e instanceof Error ? e.message : String(e) };
  }
});

// Download the latest release's plugin ZIP and install/update it into a project.
// The ZIP carries the precompiled MCP server exe; Install-Plugin.ps1's two-pass
// mount copies it in. No pre-existing in-project plugin required.
ipcMain.handle('install-plugin-from-release', async (_evt, opts: {
  uproject: string; client?: string; build?: boolean; engineDir?: string; applyPatches?: boolean;
}) => {
  if (!opts.uproject || !fs.existsSync(opts.uproject)) {
    return { ok: false, error: 'Pick a valid .uproject first.' };
  }
  // Validate the client against the known set BEFORE it becomes a script arg —
  // a value like '-SomethingElse' or 'All; …' can't smuggle extra parameters.
  const client = opts.client ?? 'All';
  if (!ALLOWED_CLIENTS.has(client)) {
    return { ok: false, error: `Unknown client '${client}'. Allowed: ${[...ALLOWED_CLIENTS].join(', ')}.` };
  }
  let tmp = '';
  try {
    logLine('Checking GitHub for the latest release ...');
    const rel = await fetchLatestRelease();
    const asset = rel.assets.find(a => /-plugin\.zip$/i.test(a.name));
    if (!asset) throw new Error(`No -plugin.zip asset in release ${rel.tag}.`);
    if (!asset.digest) throw new Error('Release asset has no integrity digest — refusing to install an unverified download.');
    tmp = path.join(app.getPath('temp'), `bpr-dl-${Date.now()}`);
    fs.mkdirSync(tmp, { recursive: true });
    const zip = path.join(tmp, asset.name);
    logLine(`Downloading ${asset.name} (${rel.tag}) ...`);
    await downloadFile(asset.url, zip, 'plugin', { size: asset.size, digest: asset.digest });
    const extract = path.join(tmp, 'x');
    logLine('Extracting ...');
    const ec = await expandArchive(zip, extract);
    if (ec !== 0) throw new Error('extraction failed.');
    const pluginRoot = findPluginRoot(extract);
    if (!pluginRoot) throw new Error('BlueprintReader.uplugin not found in the downloaded archive.');
    const installScript = path.join(pluginRoot, 'Scripts', 'Install-Plugin.ps1');
    if (!fs.existsSync(installScript)) throw new Error('Install-Plugin.ps1 missing from the archive.');

    const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', installScript,
      '-ProjectFile', opts.uproject, '-Client', client, '-Force'];
    if (opts.build) {
      if (opts.engineDir) args.push('-EngineDir', opts.engineDir);
      if (opts.applyPatches) args.push('-ApplyEnginePatches');
    } else {
      args.push('-SkipBuild');
    }
    logLine(`Installing into ${path.dirname(opts.uproject)} ...`);
    const code = await runPwsh(args);
    return { ok: code === 0, code, tag: rel.tag };
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    logLine(`[error] ${msg}`);
    return { ok: false, error: msg };
  } finally {
    if (tmp) { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ } }
  }
});

// Self-update the portable Toolbox exe: download the latest -toolbox-win64.exe,
// then a detached helper waits for this process to exit, swaps the on-disk
// portable exe, and relaunches it. electron-builder's portable target exposes
// the original exe path via PORTABLE_EXECUTABLE_FILE.
ipcMain.handle('self-update-toolbox', async () => {
  const portableExe = process.env['PORTABLE_EXECUTABLE_FILE'];
  if (!app.isPackaged || !portableExe) {
    return { ok: false, error: 'Self-update is only available in the packaged portable exe (not in dev).' };
  }
  let tmp = '';
  try {
    logLine('Checking GitHub for the latest Toolbox ...');
    const rel = await fetchLatestRelease();
    // Only update when the release is strictly newer — never auto-downgrade
    // (GitHub 'latest' can point at an older tag if a newer one is yanked).
    if (cmpVersion(rel.tag, app.getVersion()) <= 0) {
      return { ok: true, upToDate: true, tag: rel.tag };
    }
    const asset = rel.assets.find(a => /-toolbox-win64\.exe$/i.test(a.name));
    if (!asset) throw new Error(`No -toolbox-win64.exe asset in release ${rel.tag}.`);
    // Refuse to download-and-EXECUTE a binary with no integrity digest. The
    // sha256 is weak (it shares the GitHub-API trust channel) but "no check at
    // all" is the real hole — a missing digest must hard-fail, not silently skip
    // verification. (Full fix = Authenticode signing, tracked as TBX-S1 1b.)
    if (!asset.digest) {
      throw new Error('Release asset has no integrity digest — refusing to self-update an unverified binary.');
    }
    tmp = path.join(app.getPath('temp'), `bpr-tbup-${Date.now()}`);
    fs.mkdirSync(tmp, { recursive: true });
    const newExe = path.join(tmp, asset.name);
    logLine(`Downloading Toolbox ${rel.tag} ...`);
    await downloadFile(asset.url, newExe, 'Toolbox', { size: asset.size, digest: asset.digest });

    // swap helper: runs after this app exits, swaps the on-disk portable exe,
    // relaunches it. Hardened against the real failure modes the review found:
    //  - powershell.exe (Windows PowerShell 5.1, always present) not pwsh.exe
    //    (PowerShell 7 isn't on a stock Windows install). Uses only 5.1 cmdlets.
    //  - retry Copy-Item until the file is writable: process.pid is the inner
    //    Electron pid; the OUTER portable stub still holds the on-disk image lock
    //    for a variable window after, so a fixed sleep races it. Retry until the
    //    stub releases the lock.
    //  - back up the current exe and restore+relaunch it if the swap/relaunch
    //    fails, so a bad swap never bricks the app.
    //  - log to a file (stdio is detached) so a silent failure is diagnosable.
    //  - delete its own temp dir last so self-updates don't leak ~70 MB each.
    const helper = path.join(tmp, 'swap.ps1');
    fs.writeFileSync(helper, [
      'param([int]$OldPid,[string]$NewExe,[string]$CurExe)',
      '$log = Join-Path $env:TEMP "bpr-toolbox-update.log"',
      'function Log($m){ ("[" + (Get-Date -Format o) + "] " + $m) | Out-File -FilePath $log -Append -Encoding utf8 }',
      'try { Wait-Process -Id $OldPid -Timeout 120 } catch { Log "wait: $_" }',
      '$bak = "$CurExe.bak"',
      'try { Copy-Item -LiteralPath $CurExe -Destination $bak -Force -ErrorAction Stop } catch { Log "backup failed: $_" }',
      '$deadline = (Get-Date).AddSeconds(60); $copied = $false',
      'while ((Get-Date) -lt $deadline) {',
      '  try { Copy-Item -LiteralPath $NewExe -Destination $CurExe -Force -ErrorAction Stop; $copied = $true; break }',
      '  catch { Start-Sleep -Milliseconds 250 }',
      '}',
      '$relaunched = $false',
      'if ($copied) {',
      '  Log "swapped ok"',
      '  try { $proc = Start-Process -FilePath $CurExe -PassThru; Start-Sleep -Seconds 2; if ($proc -and -not $proc.HasExited) { $relaunched = $true } else { Log "new exe exited within 2s of launch" } } catch { Log "relaunch of new exe failed: $_" }',
      '  if (-not $relaunched -and (Test-Path $bak)) {',
      '    Log "new exe would not launch; restoring original from backup"',
      '    try { Copy-Item -LiteralPath $bak -Destination $CurExe -Force; Start-Process -FilePath $CurExe } catch { Log "restore failed: $_" }',
      '  }',
      '} else {',
      '  Log "swap failed after 60s (exe locked); restoring original"',
      '  if (Test-Path $bak) { try { Copy-Item -LiteralPath $bak -Destination $CurExe -Force } catch { Log "restore copy failed: $_" } }',
      '  try { Start-Process -FilePath $CurExe } catch { Log "relaunch original failed: $_" }',
      '}',
      '# R5: only discard the backup after a verified swap+relaunch of the NEW exe;',
      '# otherwise keep it so a half-overwritten exe stays recoverable.',
      'if ($copied -and $relaunched) { Remove-Item -LiteralPath $bak -Force -ErrorAction SilentlyContinue } else { Log "keeping backup at $bak" }',
      'Remove-Item -LiteralPath (Split-Path -Parent $NewExe) -Recurse -Force -ErrorAction SilentlyContinue',
    ].join('\n'), 'utf8');

    const child = spawn('powershell.exe', ['-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass',
      '-File', helper, '-OldPid', String(process.pid), '-NewExe', newExe, '-CurExe', portableExe],
      { detached: true, stdio: 'ignore' });
    // Only quit once the helper has actually launched — otherwise a spawn failure
    // would leave the app closed with no update applied and no error shown.
    const started = await new Promise<boolean>((resolve) => {
      let settled = false;
      child.once('spawn', () => { if (!settled) { settled = true; resolve(true); } });
      child.once('error', (e) => { if (!settled) { settled = true; logLine(`[error] update helper failed to launch: ${e.message}`); resolve(false); } });
    });
    if (!started) {
      try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ }
      return { ok: false, error: 'Could not launch the update helper. The Toolbox was NOT updated and is still running.' };
    }
    child.unref();
    logLine('Toolbox update downloaded — restarting to apply ...');
    setTimeout(() => app.quit(), 900);
    return { ok: true, tag: rel.tag };
  } catch (e) {
    // On any failure before the detached helper took ownership of tmp, remove it.
    if (tmp) { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ } }
    const msg = e instanceof Error ? e.message : String(e);
    logLine(`[error] ${msg}`);
    return { ok: false, error: msg };
  }
});

// Deploy the AI-assistant assets (AGENTS.md, .github/copilot-instructions.md,
// Claude skills/agents) into a project. Runs the in-project Install-ClaudeAssets.ps1
// if the plugin is already mounted; otherwise downloads the latest release and
// runs the extracted copy — so "Deploy Assets" works even before install.
ipcMain.handle('deploy-assets', async (_evt, opts: { projectDir: string }) => {
  const projectDir = opts?.projectDir;
  if (!projectDir) return { ok: false, error: 'No project configured — set one on the Install tab first.' };
  const inProject = path.join(projectDir, 'Plugins', 'BlueprintReader', 'Scripts', 'Install-ClaudeAssets.ps1');
  if (fs.existsSync(inProject)) {
    const code = await runPwsh(['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', inProject, '-ProjectRoot', projectDir]);
    return { ok: code === 0, code };
  }
  let tmp = '';
  try {
    logLine('Plugin not mounted here — fetching assets from the latest release ...');
    const rel = await fetchLatestRelease();
    const asset = rel.assets.find(a => /-plugin\.zip$/i.test(a.name));
    if (!asset) throw new Error(`No -plugin.zip asset in release ${rel.tag}.`);
    if (!asset.digest) throw new Error('Release asset has no integrity digest — refusing to install an unverified download.');
    tmp = path.join(app.getPath('temp'), `bpr-assets-${Date.now()}`);
    fs.mkdirSync(tmp, { recursive: true });
    const zip = path.join(tmp, asset.name);
    await downloadFile(asset.url, zip, 'plugin', { size: asset.size, digest: asset.digest });
    const extract = path.join(tmp, 'x');
    const ec = await expandArchive(zip, extract);
    if (ec !== 0) throw new Error('extraction failed.');
    const pluginRoot = findPluginRoot(extract);
    if (!pluginRoot) throw new Error('BlueprintReader.uplugin not found in the archive.');
    const script = path.join(pluginRoot, 'Scripts', 'Install-ClaudeAssets.ps1');
    if (!fs.existsSync(script)) throw new Error('Install-ClaudeAssets.ps1 missing from the archive.');
    const code = await runPwsh(['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-ProjectRoot', projectDir]);
    return { ok: code === 0, code };
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    logLine(`[error] ${msg}`);
    return { ok: false, error: msg };
  } finally {
    if (tmp) { try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ } }
  }
});

// Kill any orphaned BlueprintReader MCP processes: every BlueprintReaderMcp.exe,
// plus any UnrealEditor-Cmd.exe running the BPR daemon (-run=BPR) — but NOT a
// normal editor. Returns how many were terminated.
// TBX-R4: scoped by default to THIS project's server exe (+ a BPR daemon whose
// command line references this project) so one project's Toolbox can't kill
// another project's / a CI run's server. `global:true` is the explicit opt-in
// for a machine-wide sweep (every BlueprintReaderMcp.exe).
ipcMain.handle('kill-mcp-servers', (_evt, opts?: { global?: boolean }) => {
  const global = opts?.global === true;
  // Always drop our own tracked handles so the Tester UI reflects the kill.
  for (const [pid, proc] of serverProcesses) { try { proc.kill(); } catch { /* ignore */ } serverProcesses.delete(pid); }
  const projectDir = getProjectDir();
  const exePath = projectDir ? getExePath(projectDir) : '';
  // When scoped, match the MCP server on its exact ExecutablePath and the BPR
  // daemon on a command line that references this project dir.
  const mcpFilter = global || !exePath
    ? '$mcp = Get-CimInstance Win32_Process -Filter "Name=\'BlueprintReaderMcp.exe\'" -ErrorAction SilentlyContinue'
    : '$mcp = Get-CimInstance Win32_Process -Filter "Name=\'BlueprintReaderMcp.exe\'" -ErrorAction SilentlyContinue | Where-Object { $_.ExecutablePath -ieq $env:BPR_EXE }';
  const daemonFilter = global || !projectDir
    ? '$d = Get-CimInstance Win32_Process -Filter "Name=\'UnrealEditor-Cmd.exe\'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match "-run=BPR" }'
    : '$d = Get-CimInstance Win32_Process -Filter "Name=\'UnrealEditor-Cmd.exe\'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match "-run=BPR" -and $_.CommandLine.Contains($env:BPR_PROJDIR) }';
  const ps = [
    '$n = 0',
    mcpFilter,
    'foreach ($p in $mcp) { try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop; $n++ } catch {} }',
    daemonFilter,
    'foreach ($p in $d) { try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop; $n++ } catch {} }',
    'Write-Output "BPRKILLED=$n"',
  ].join('; ');
  return new Promise((resolve) => {
    let out = '';
    const p = trackChild(spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
      { windowsHide: true, env: { ...process.env, BPR_EXE: exePath, BPR_PROJDIR: projectDir } }));
    p.stdout.on('data', (d: Buffer) => { out += d.toString('utf8'); });
    p.stderr.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.on('exit', () => {
      const m = out.match(/BPRKILLED=(\d+)/);
      const count = m ? parseInt(m[1], 10) : 0;
      logLine(`Stopped ${count} BlueprintReader MCP process(es).`);
      resolve({ ok: true, count });
    });
    p.on('error', (e) => { logLine(`[error] ${e.message}`); resolve({ ok: false, error: e.message }); });
  });
});

// Script runner — streams stdout via 'script-log' event, resolves with exit code
ipcMain.handle('run-script', (_evt, script: string, args: string[]) => {
  return new Promise<number>((resolve) => {
    // The ONLY script the renderer legitimately runs is the plugin's
    // Scripts/Generate-ClientConfig.ps1, so constrain to that dir (realpath-
    // checked, so a junction can't escape it). Release-extracted scripts run via
    // the dedicated install/deploy IPCs (runPwsh), not through here.
    const projectDir = getProjectDir();
    const scriptsDir = projectDir ? path.join(getPluginDir(projectDir), 'Scripts') : '';
    if (!scriptsDir || !script.toLowerCase().endsWith('.ps1') || !isPathUnder(script, [scriptsDir])) {
      mainWindow?.webContents.send('script-log',
        `[error] refused to run a script outside the plugin Scripts dir: ${script}\n`);
      resolve(1);
      return;
    }
    const proc = trackChild(spawn('pwsh.exe', [
      '-NoProfile',
      '-ExecutionPolicy', 'Bypass',
      '-File', script,
      ...args,
    ], { windowsHide: true }));

    const sendLog = (data: Buffer) => {
      const text = data.toString('utf8');
      mainWindow?.webContents.send('script-log', text);
    };

    proc.stdout.on('data', sendLog);
    proc.stderr.on('data', sendLog);
    proc.on('exit', (code) => resolve(code ?? 0));
    proc.on('error', (err) => {
      mainWindow?.webContents.send('script-log', `[error] ${err.message}\n`);
      resolve(1);
    });
  });
});

// Server lifecycle
const serverProcesses = new Map<number, ChildProcess>();

// TBX-R3: forward the env as a DENYLIST, not an allowlist. The commandlet
// backend's server spawns UnrealEditor-Cmd.exe, which inherits the server's env
// verbatim — UE + Windows DLL loading rely on the full system env (ProgramData,
// ProgramFiles*, USERNAME, COMPUTERNAME, …), so a tight allowlist would break a
// real editor launch. We drop only obviously-sensitive keys (tokens/secrets/
// credentials the server never needs); the renderer's BP_READER_* is layered on.
const SENSITIVE_ENV_RE = /token|secret|passwo?rd|passwd|api[_-]?key|access[_-]?key|client[_-]?secret|credential|auth/i;

// TBX-R1: cancel in-flight transient operations (install/extract/script/kill)
// by tearing down their process trees. The server lifecycle is separate.
ipcMain.handle('cancel-operation', () => {
  for (const c of activeDownloads) c.abort();
  activeDownloads.clear();
  killAllTransient();
});

ipcMain.handle('start-server', (_evt, opts: {
  backend: string;
  port: number;
  env: Record<string, string>;
}) => {
  return new Promise<number>((resolve, reject) => {
    const projectDir = getProjectDir();
    const exePath = getExePath(projectDir);

    if (!fs.existsSync(exePath)) {
      reject(new Error(`Server exe not found: ${exePath}. Build the plugin first.`));
      return;
    }

    // TBX-R3: forward only a whitelisted slice of the main-process env (not the
    // whole thing), then layer the renderer-supplied BP_READER_* on top. We no
    // longer hardcode BP_READER_READ_ONLY:'0' — that silently defeated the
    // read-only-by-default invariant; write mode is now an explicit user choice
    // (set BP_READER_ALLOW_WRITE=1 in Settings, which flows through opts.env).
    const baseEnv: NodeJS.ProcessEnv = {};
    for (const [k, v] of Object.entries(process.env)) {
      if (v !== undefined && !SENSITIVE_ENV_RE.test(k)) baseEnv[k] = v;
    }
    const env: NodeJS.ProcessEnv = {
      ...baseEnv,
      BP_READER_BACKEND: opts.backend,
      BP_READER_HTTP_PORT: String(opts.port),
      ...opts.env,
    };

    const proc = spawn(exePath, [], { env, windowsHide: true });
    proc.on('error', (e) => {
      if (proc.pid) serverProcesses.delete(proc.pid);
      reject(e);
    });
    if (!proc.pid) {
      reject(new Error('Failed to spawn server process'));
      return;
    }
    serverProcesses.set(proc.pid, proc);
    proc.on('exit', () => {
      if (proc.pid) serverProcesses.delete(proc.pid);
    });
    // Give the server 500ms to start listening — but only resolve the PID if it
    // is still alive (a crash-on-start otherwise resolved a dead PID).
    setTimeout(() => {
      if (proc.exitCode !== null || proc.signalCode !== null) {
        reject(new Error(`Server exited immediately (code ${proc.exitCode ?? proc.signalCode}). Check the backend/env and exe.`));
        return;
      }
      resolve(proc.pid!);
    }, 500);
  });
});

ipcMain.handle('stop-server', (_evt, pid: number) => {
  const proc = serverProcesses.get(pid);
  if (proc) {
    proc.kill();
    serverProcesses.delete(pid);
  }
});

ipcMain.handle('is-running', (_evt, pid: number) => {
  const proc = serverProcesses.get(pid);
  if (!proc) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
});
