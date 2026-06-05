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

app.whenReady().then(createWindow);
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });

// ---------------------------------------------------------------------------
// IPC handlers
// ---------------------------------------------------------------------------

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
