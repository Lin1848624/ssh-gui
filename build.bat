@echo off
rem ===========================================================================
rem  SshGui - build script (MSVC, static CRT, single exe output)
rem  Usage:  build.bat          -> Release
rem          build.bat debug    -> Debug
rem          build.bat clean    -> remove obj/ bin/
rem ===========================================================================
setlocal enabledelayedexpansion
pushd "%~dp0"

if /i "%~1"=="clean" (
    if exist "obj" rmdir /s /q "obj"
    if exist "bin" rmdir /s /q "bin"
    echo CLEAN OK
    popd & endlocal & exit /b 0
)

set "CFG=release"
if /i "%~1"=="debug" set "CFG=debug"

rem ---------- locate MSVC build environment ----------
set "VCVARS="
if exist "D:\App\VS-BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=D:\App\VS-BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)
if not defined VCVARS (
    echo [ERROR] vcvars64.bat not found. Install "Visual Studio Build Tools"
    echo         with the "Desktop development with C++" workload.
    popd & endlocal & exit /b 1
)

echo [1/3] Initializing MSVC environment ...
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    popd & endlocal & exit /b 1
)

if not exist "bin" mkdir "bin"
if not exist "obj" mkdir "obj"

echo [2/3] Compiling resources ...
rc /nologo /fo "obj\app.res" /I "res" "res\app.rc"
if errorlevel 1 (
    echo [ERROR] resource compilation failed
    popd & endlocal & exit /b 1
)

echo [3/3] Compiling and linking ...
set "OPT=/O2 /MT /DNDEBUG /GS"
set "WLV=/W3"
if /i "%CFG%"=="debug" set "OPT=/Od /MTd /D_DEBUG /Zi /GS"
if /i "%~2"=="w4" set "WLV=/W4"

cl /nologo /std:c++17 /EHsc /utf-8 %WLV% %OPT% ^
   /Fo"obj\\" /Fe"bin\SshGui.exe" ^
   "src\*.cpp" "obj\app.res" ^
   /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO ^
   /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
if errorlevel 1 (
    echo [ERROR] build failed
    popd & endlocal & exit /b 1
)

echo.
echo BUILD OK -^> %~dp0bin\SshGui.exe
popd
endlocal
exit /b 0
