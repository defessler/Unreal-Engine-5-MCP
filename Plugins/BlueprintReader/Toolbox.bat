@echo off
set "PLUGIN_DIR=%~dp0"
set "TOOLBOX_PROJECT_DIR=%~dp0..\.."
if not exist "%PLUGIN_DIR%Binaries\Toolbox\BlueprintReader Toolbox.exe" (
    echo BlueprintReader Toolbox is not built yet.
    echo Run: cd Plugins\BlueprintReader\Toolbox ^&^& npm run dist
    pause
    exit /b 1
)
start "" "%PLUGIN_DIR%Binaries\Toolbox\BlueprintReader Toolbox.exe" --project-dir="%TOOLBOX_PROJECT_DIR%"
