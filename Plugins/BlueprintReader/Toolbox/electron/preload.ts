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
  saveProject(uprojectPath: string): Promise<void> {
    return ipcRenderer.invoke('save-project', uprojectPath);
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
};

contextBridge.exposeInMainWorld('electronAPI', api);

export type ElectronAPI = typeof api;
