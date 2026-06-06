import {
  app,
  BrowserWindow,
  ipcMain,
  dialog,
  shell,
  Menu,
} from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { pipeline } from 'node:stream/promises';
import { Readable, Transform } from 'node:stream';
import { spawn, execSync, ChildProcess } from 'child_process';

const isDev = !app.isPackaged;

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
    const out = execSync(`reg query "${key}" /v "${name}" 2>nul`, { encoding: 'utf8', timeout: 3000 });
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
    } else {
      // Version string -> HKLM
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

function createWindow() {
  // Remove the native File/Edit/View/Window/Help menu bar entirely.
  Menu.setApplicationMenu(null);

  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    minWidth: 900,
    minHeight: 600,
    backgroundColor: '#1a1a1a',
    frame: false,          // custom title bar in renderer
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
    title: 'BlueprintReader Toolbox',
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools();
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
    // Clean up any spawned servers
    for (const [, proc] of serverProcesses) {
      try { proc.kill(); } catch { /* ignore */ }
    }
  });
}

// Reap stale temp dirs from prior installs / self-updates. The self-update path
// intentionally can't delete its own dir before quitting (the detached helper
// still needs it), and a killed install could orphan one too. A dir a fresh
// helper still holds is simply skipped and retried next launch.
function sweepStaleTempDirs() {
  try {
    const tmpRoot = app.getPath('temp');
    for (const name of fs.readdirSync(tmpRoot)) {
      if (/^bpr-(tbup|dl)-/.test(name)) {
        try { fs.rmSync(path.join(tmpRoot, name), { recursive: true, force: true }); } catch { /* in use; next time */ }
      }
    }
  } catch { /* ignore */ }
}

app.whenReady().then(() => { sweepStaleTempDirs(); createWindow(); });
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
  saveProject(uprojectPath);
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

ipcMain.handle('read-file', async (_evt, filePath: string) => {
  try {
    return fs.readFileSync(filePath, 'utf8');
  } catch {
    return null;
  }
});

ipcMain.handle('write-file', async (_evt, filePath: string, content: string) => {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, content, 'utf8');
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
  await shell.openExternal(url);
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

// Compare dot-separated numeric versions. >0 if a newer than b, <0 if older, 0 equal.
// Strips a leading 'v' and any pre-release suffix. Used so self-update never
// applies a same-or-older release (downgrade guard).
function cmpVersion(a: string, b: string): number {
  const parse = (s: string) => (s ?? '').replace(/^v/, '').split('-')[0].split('.').map(n => parseInt(n, 10) || 0);
  const pa = parse(a), pb = parse(b);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d > 0 ? 1 : -1;
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
async function downloadFile(
  url: string, destPath: string, label: string,
  expected?: { size?: number; digest?: string },
): Promise<void> {
  const ctrl = new AbortController();
  let stall: NodeJS.Timeout | undefined;
  const STALL_MS = 60000;
  const arm = () => { if (stall) clearTimeout(stall); stall = setTimeout(() => ctrl.abort(), STALL_MS); };
  arm();
  let got = 0, lastPct = -1;
  try {
    const res = await fetch(url, { headers: { 'User-Agent': 'bp-reader-toolbox' }, redirect: 'follow', signal: ctrl.signal });
    if (!res.ok || !res.body) throw new Error(`download of ${label} failed: HTTP ${res.status}`);
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
    await pipeline(Readable.fromWeb(res.body as Parameters<typeof Readable.fromWeb>[0]), progress, fs.createWriteStream(destPath));

    const wantSize = expected?.size || total;
    if (wantSize && got !== wantSize) {
      try { fs.rmSync(destPath, { force: true }); } catch { /* ignore */ }
      throw new Error(`${label} download incomplete: ${got}/${wantSize} bytes (connection dropped?)`);
    }
    if (hash && expected?.digest) {
      const want = expected.digest.replace(/^sha256:/i, '').toLowerCase();
      const gotHash = hash.digest('hex');
      if (gotHash !== want) {
        try { fs.rmSync(destPath, { force: true }); } catch { /* ignore */ }
        throw new Error(`${label} integrity check failed (sha256 mismatch — corrupt download)`);
      }
    }
    logLine(`  ${label}: downloaded ${(got / 1048576).toFixed(1)} MB`);
  } finally {
    if (stall) clearTimeout(stall);
  }
}

function runPwsh(args: string[]): Promise<number> {
  return new Promise((resolve) => {
    const p = spawn('pwsh.exe', args, { windowsHide: true });
    p.stdout.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.stderr.on('data', (d: Buffer) => logLine(d.toString('utf8')));
    p.on('exit', (c) => resolve(c ?? 0));
    p.on('error', (e) => { logLine(`[error] ${e.message}`); resolve(1); });
  });
}

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
  let tmp = '';
  try {
    logLine('Checking GitHub for the latest release ...');
    const rel = await fetchLatestRelease();
    const asset = rel.assets.find(a => /-plugin\.zip$/i.test(a.name));
    if (!asset) throw new Error(`No -plugin.zip asset in release ${rel.tag}.`);
    tmp = path.join(app.getPath('temp'), `bpr-dl-${Date.now()}`);
    fs.mkdirSync(tmp, { recursive: true });
    const zip = path.join(tmp, asset.name);
    logLine(`Downloading ${asset.name} (${rel.tag}) ...`);
    await downloadFile(asset.url, zip, 'plugin', { size: asset.size, digest: asset.digest });
    const extract = path.join(tmp, 'x');
    logLine('Extracting ...');
    const ec = await runPwsh(['-NoProfile', '-Command',
      `Expand-Archive -LiteralPath '${zip.replace(/'/g, "''")}' -DestinationPath '${extract.replace(/'/g, "''")}' -Force`]);
    if (ec !== 0) throw new Error('extraction failed.');
    const pluginRoot = findPluginRoot(extract);
    if (!pluginRoot) throw new Error('BlueprintReader.uplugin not found in the downloaded archive.');
    const installScript = path.join(pluginRoot, 'Scripts', 'Install-Plugin.ps1');
    if (!fs.existsSync(installScript)) throw new Error('Install-Plugin.ps1 missing from the archive.');

    const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', installScript,
      '-ProjectFile', opts.uproject, '-Client', opts.client ?? 'All', '-Force'];
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
      'if (-not $copied) {',
      '  Log "swap failed after 60s (exe locked); restoring original"',
      '  if (Test-Path $bak) { try { Copy-Item -LiteralPath $bak -Destination $CurExe -Force } catch {} }',
      '} else { Log "swapped ok" }',
      'try { Start-Process -FilePath $CurExe } catch { Log "relaunch failed: $_" }',
      'Remove-Item -LiteralPath $bak -Force -ErrorAction SilentlyContinue',
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

// Script runner — streams stdout via 'script-log' event, resolves with exit code
ipcMain.handle('run-script', (_evt, script: string, args: string[]) => {
  return new Promise<number>((resolve) => {
    const proc = spawn('pwsh.exe', [
      '-NoProfile',
      '-ExecutionPolicy', 'Bypass',
      '-File', script,
      ...args,
    ], { windowsHide: true });

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

    const env: NodeJS.ProcessEnv = {
      ...process.env,
      BP_READER_BACKEND: opts.backend,
      BP_READER_HTTP_PORT: String(opts.port),
      BP_READER_READ_ONLY: '0',
      ...opts.env,
    };

    const proc = spawn(exePath, [], { env, windowsHide: true });
    if (!proc.pid) {
      reject(new Error('Failed to spawn server process'));
      return;
    }
    serverProcesses.set(proc.pid, proc);
    proc.on('exit', () => {
      if (proc.pid) serverProcesses.delete(proc.pid);
    });
    // Give the server 500ms to start listening before resolving
    setTimeout(() => resolve(proc.pid!), 500);
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
