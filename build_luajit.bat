@echo off
setlocal
rem builds third_party\luajit (lua51.lib + lua51.dll). run once before building
rem the app; the main build links the import lib and copies the dll post-build.

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo error: vswhere.exe not found. install visual studio.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set VCVARS=%%i
if "%VCVARS%"=="" (
    echo error: vcvars64.bat not found.
    exit /b 1
)

call "%VCVARS%" >nul
cd /d "%~dp0third_party\luajit\src"
call msvcbuild.bat
exit /b %ERRORLEVEL%
