@echo off
rem One-click build + run (ASCII only, cmd code page independent)
setlocal
set "CMAKE=E:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD=%~dp0build"
set "EXE=%BUILD%\Debug\sandbox_engine.exe"

echo [1/2] Building...
"%CMAKE%" --build "%BUILD%" --config Debug
if errorlevel 1 (
    echo [ERROR] build failed - fix code first
    exit /b 1
)

echo [2/2] Launching...
start "" "%EXE%"
echo Done. Window opened (chdir auto-set to exe dir, shaders/textures load).
endlocal
