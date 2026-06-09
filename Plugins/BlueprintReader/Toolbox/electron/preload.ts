import { contextBridge, ipcRenderer } from 'electron';

export interface PathInfo {
  projectDir: string;
  pluginDir: string;
  exePath: string;
  engineDir: string;
  uproject: string;
}

export interface StartServerOpts {
  backend: string;
  port: number;
  env: Record<string, string>;
}

export interface OpenDialogOpts {
  title?: string;
  defaultPath?: string;
  filters?: { name: string; extensions: string[] }[];
  properties?: Array<'openFile' | 'openDirectory'>;
}

export interface LatestRelease {
  ok: boolean;
  tag?: string;
  hasPlugin?: boolean;
  hasToolbox?: boolean;
  error?: string;
}

export interface InstallFromReleaseOpts {
  uproject: string;
  client?: string;
  build?: boolean;
  engineDir?: string;
  applyPatches?: boolean;
}

export interface InstallResult { ok: boolean; code?: number; tag?: string; error?: string; }
export interface SelfUpdateResult { ok: boolean; tag?: string; upToDate?: boolean; error?: string; }
export interface DeployResult { ok: boolean; code?: number; error?: string; }
export interface KillResult { ok: boolean; count?: number; error?: string; }

const api = {
  getPaths(): Promise<PathInfo> {
    return ipcRenderer.invoke('get-paths');
  },
  readFile(path: string): Promise<string | null> {
    return ipcRenderer.invoke('read-file', path);
  },
  writeFile(path: string, content: string): Promise<void> {
    return ipcRenderer.invoke('write-file', path, content);
  },
  openFileDialog(opts: OpenDialogOpts): Promise<string | null> {
    return ipcRenderer.invoke('open-file-dialog', opts);
  },
  openExternal(url: string): Promise<void> {
    return ipcRenderer.invoke('open-external', url);
  },
  runScript(script: string, args: string[]): Promise<number> {
    return ipcRenderer.invoke('run-script', script, args);
  },
  onScriptLog(cb: (line: string) => void): () => void {
    const handler = (_: Electron.IpcRendererEvent, line: string) => cb(line);
    ipcRenderer.on('script-log', handler);
    return () => ipcRenderer.removeListener('script-log', handler);
  },
  startServer(opts: StartServerOpts): Promise<number> {
    return ipcRenderer.invoke('start-server', opts);
  },
  stopServer(pid: number): Promise<void> {
    return ipcRenderer.invoke('stop-server', pid);
  },
  isRunning(pid: number): Promise<boolean> {
    return ipcRenderer.invoke('is-running', pid);
  },
  getAppVersion(): Promise<string> {
    return ipcRenderer.invoke('get-app-version');
  },
  getEnvPaths(): Promise<{ userProfile: string; appData: string }> {
    return ipcRenderer.invoke('get-env-paths');
  },
  saveProject(uprojectPath: string): Promise<void> {
    return ipcRenderer.invoke('save-project', uprojectPath);
  },
  uprojectExists(uprojectPath: string): Promise<boolean> {
    return ipcRenderer.invoke('uproject-exists', uprojectPath);
  },
  minimizeWindow(): Promise<void> {
    return ipcRenderer.invoke('minimize-window');
  },
  maximizeWindow(): Promise<void> {
    return ipcRenderer.invoke('maximize-window');
  },
  closeWindow(): Promise<void> {
    return ipcRenderer.invoke('close-window');
  },
  resolveEngine(uprojectPath: string): Promise<string> {
    return ipcRenderer.invoke('resolve-engine', uprojectPath);
  },
  getLatestRelease(): Promise<LatestRelease> {
    return ipcRenderer.invoke('get-latest-release');
  },
  installPluginFromRelease(opts: InstallFromReleaseOpts): Promise<InstallResult> {
    return ipcRenderer.invoke('install-plugin-from-release', opts);
  },
  selfUpdateToolbox(): Promise<SelfUpdateResult> {
    return ipcRenderer.invoke('self-update-toolbox');
  },
  deployAssets(opts: { projectDir: string }): Promise<DeployResult> {
    return ipcRenderer.invoke('deploy-assets', opts);
  },
  killMcpServers(opts?: { global?: boolean }): Promise<KillResult> {
    return ipcRenderer.invoke('kill-mcp-servers', opts);
  },
  cancelOperation(): Promise<void> {
    return ipcRenderer.invoke('cancel-operation');
  },
};

contextBridge.exposeInMainWorld('electronAPI', api);

export type ElectronAPI = typeof api;
