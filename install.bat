@echo off
title Vietcong DirectPlay host-fix installer

rem ---------------------------------------------------------------------------
rem  Installs the host-fix dpnet.dll as the DirectPlay8 COM provider for the
rem  current user (no admin required, no system files touched).
rem
rem  The shim does NOT replace DirectPlay - it delegates every call to the real
rem  system dpnet.dll and only patches the listen-host self-join in-process.
rem  ONLY THE PERSON HOSTING needs this; everyone else uses stock DirectPlay
rem  unchanged and stays fully interoperable.
rem
rem  It copies dpnet.dll to %LOCALAPPDATA% and points the four DirectPlay8
rem  CLSIDs at it under HKCU. 32-bit games read the Wow6432Node path, so both
rem  are registered. Run uninstall.bat to revert.
rem ---------------------------------------------------------------------------

set "INSTALL_DIR=%LOCALAPPDATA%\vietcong-directplay-hostfix"

rem Accept dpnet.dll either in a bin\ subfolder or right next to this script.
set "SRC_DLL=%~dp0bin\dpnet.dll"
if not exist "%SRC_DLL%" set "SRC_DLL=%~dp0dpnet.dll"

if not exist "%SRC_DLL%" (
    echo [ERROR] dpnet.dll was not found.
    echo Keep install.bat together with dpnet.dll ^(or the bin\ folder^) and
    echo run it from there. You do NOT need to copy anything into the game folder.
    echo.
    pause
    exit /b 1
)

echo Installing the Vietcong DirectPlay host-fix...
echo.

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /Y "%SRC_DLL%" "%INSTALL_DIR%\dpnet.dll" >nul
if errorlevel 1 (
    echo [ERROR] Could not copy dpnet.dll to "%INSTALL_DIR%".
    echo.
    pause
    exit /b 1
)
set "DLL=%INSTALL_DIR%\dpnet.dll"

rem DirectPlay8 CLSIDs: Peer, Server, Client, Address
for %%G in (
    {286F484D-375E-4458-A272-B138E2F80A6A}
    {DA825E1B-6830-43D7-835D-0B5AD82956A2}
    {743F1DC6-5ABA-429F-8BDF-C54D03253DC2}
    {934A9523-A3CA-4BC5-ADA0-D6D95D979421}
) do (
    reg add "HKCU\Software\Classes\CLSID\%%G\InprocServer32" /ve /t REG_SZ /d "%DLL%" /f >nul
    reg add "HKCU\Software\Classes\CLSID\%%G\InprocServer32" /v ThreadingModel /t REG_SZ /d Both /f >nul
    reg add "HKCU\Software\Classes\Wow6432Node\CLSID\%%G\InprocServer32" /ve /t REG_SZ /d "%DLL%" /f >nul
    reg add "HKCU\Software\Classes\Wow6432Node\CLSID\%%G\InprocServer32" /v ThreadingModel /t REG_SZ /d Both /f >nul
)

echo Done. DirectPlay8 is now provided by:
echo     %DLL%
echo.
echo You can now host a LAN or Internet game in Vietcong.
echo Other players do NOT need this - only the host.
echo To revert, run uninstall.bat.
echo.
pause
