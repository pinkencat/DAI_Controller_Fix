@echo off
rem Builds the 64-bit xinput9_1_0.dll proxy. Needs Visual Studio C++ build tools.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W4 /GS- /MT /LD xinput_remap.c /Fe:xinput9_1_0.dll /link /DEF:xinput9_1_0.def /NOLOGO kernel32.lib user32.lib || exit /b 1
del /q xinput_remap.obj xinput9_1_0.lib xinput9_1_0.exp 2>nul
echo Built %~dp0xinput9_1_0.dll
