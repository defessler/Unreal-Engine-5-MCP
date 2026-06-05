import type { ElectronAPI, PathInfo, StartServerOpts, OpenDialogOpts } from '../../electron/preload';

// Re-export types for use in components
export type { PathInfo, StartServerOpts, OpenDialogOpts };

function api(): ElectronAPI {
  const w = window as unknown as { electronAPI: ElectronAPI };
  if (!w.electronAPI) {
    // Return a stub for browser-only dev (outside Electron)
    return {
      getPaths: async () => ({
        projectDir: 'C:\\Projects\\MyGame',
        pluginDir: 'C:\\Projects\\MyGame\\Plugins\\BlueprintReader',
        exePath: 'C:\\Projects\\MyGame\\Plugins\\BlueprintReader\\Binaries\\Win64\\BlueprintReaderMcp.exe',
        engineDir: 'C:\\Program Files\\Epic Games\\UE_5.8',
        uproject: 'C:\\Projects\\MyGame\\MyGame.uproject',
      }),
      readFile: async () => null,
      writeFile: async () => undefined,
      openFileDialog: async () => null,
      openExternal: async () => undefined,
      runScript: async () => 0,
      onScriptLog: () => () => undefined,
      startServer: async () => 0,
      stopServer: async () => undefined,
      isRunning: async () => false,
      getAppVersion: async () => '0.0.0',
      getEnvPaths: async () => ({ userProfile: 'C:\\Users\\user', appData: 'C:\\Users\\user\\AppData\\Roaming' }),
      saveProject: async () => undefined,
      minimizeWindow: async () => undefined,
      maximizeWindow: async () => undefined,
      closeWindow: async () => undefined,
      resolveEngine: async () => '',
    };
  }
  return w.electronAPI;
}

export const bridge = {
  getPaths: () => api().getPaths(),
  readFile: (p: string) => api().readFile(p),
  writeFile: (p: string, c: string) => api().writeFile(p, c),
  openFileDialog: (o: OpenDialogOpts) => api().openFileDialog(o),
  openExternal: (url: string) => api().openExternal(url),
  runScript: (script: string, args: string[]) => api().runScript(script, args),
  onScriptLog: (cb: (line: string) => void) => api().onScriptLog(cb),
  startServer: (opts: StartServerOpts) => api().startServer(opts),
  stopServer: (pid: number) => api().stopServer(pid),
  isRunning: (pid: number) => api().isRunning(pid),
  getAppVersion: () => api().getAppVersion(),
  getEnvPaths: () => api().getEnvPaths(),
  saveProject: (uprojectPath: string) => api().saveProject(uprojectPath),
  minimizeWindow: () => api().minimizeWindow(),
  maximizeWindow: () => api().maximizeWindow(),
  closeWindow: () => api().closeWindow(),
  resolveEngine: (uprojectPath: string) => api().resolveEngine(uprojectPath),
};
