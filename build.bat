@echo off
setlocal

set CONFIG=%1
set PLATFORM=%2
if "%CONFIG%"=="" set CONFIG=Release
if "%PLATFORM%"=="" set PLATFORM=x64

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo error: vswhere.exe not found. install visual studio.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
    echo error: MSBuild.exe not found.
    exit /b 1
)

echo building guitarpedal [%CONFIG%^|%PLATFORM%]
"%MSBUILD%" guitarpedal.sln /nologo /m /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
exit /b %ERRORLEVEL%
