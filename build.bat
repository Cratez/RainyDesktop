@echo off
rem Builds raindesk with Visual Studio 2022. Output: build\Release\raindesk.exe
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with the "Desktop development with C++" workload was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Built build\Release\raindesk.exe  (quit it with Ctrl+Alt+Q)
