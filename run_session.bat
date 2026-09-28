@echo off
SETLOCAL EnableDelayedExpansion

echo =======================================================
echo  Automated Compiling and Sequencing Script (No Thunks)
echo =======================================================

taskkill /f /im win32_debugger_symbols.exe /im target.exe 2>nul >nul
timeout /t 1 /nobreak >nul

if exist win32_debugger_symbols.exe del /f win32_debugger_symbols.exe
if exist target.exe del /f target.exe
if exist target.pdb del /f target.pdb

where cl.exe >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [-] Error: cl.exe not discovered in current path.
    pause
    exit /b 1
)

echo [*] Compiling Custom Win32 Symbol Debugger (x86)...
cl.exe /nologo /O2 /Fewin32_debugger_symbols.exe win32_debugger_symbols.c dbghelp.lib /link /MACHINE:X86
if %ERRORLEVEL% NEQ 0 ( echo [-] Compilation failed. & pause & exit /b 1 )

echo [*] Compiling Target Process Application with symbols (x86)...
:: CRITICAL ENGINE FIX: /INCREMENTAL:NO prevents the linker from routing function symbols
:: through a temporary JMP thunk table, aligning the PDB to true instruction offsets.
cl.exe /nologo /Zi /Od /Gd /Fetarget.exe target.c /link /INCREMENTAL:NO /MACHINE:X86
if %ERRORLEVEL% NEQ 0 ( echo [-] Compilation failed. & pause & exit /b 1 )

timeout /t 1 /nobreak >nul

echo =======================================================
echo  Starting Interactive Debug Loop Session
echo =======================================================

win32_debugger_symbols.exe target.exe

pause
