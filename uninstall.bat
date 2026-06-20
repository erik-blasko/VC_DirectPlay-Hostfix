@echo off
title Vietcong DirectPlay host-fix uninstaller

rem ---------------------------------------------------------------------------
rem  Removes the per-user DirectPlay8 CLSID overrides installed by install.bat
rem  and deletes the copied dpnet.dll. The system DirectPlay registration in
rem  HKLM is never touched, so Vietcong falls back to it afterwards.
rem ---------------------------------------------------------------------------

set "INSTALL_DIR=%LOCALAPPDATA%\vietcong-directplay-hostfix"

echo Removing the Vietcong DirectPlay host-fix...
echo.

for %%G in (
    {286F484D-375E-4458-A272-B138E2F80A6A}
    {DA825E1B-6830-43D7-835D-0B5AD82956A2}
    {743F1DC6-5ABA-429F-8BDF-C54D03253DC2}
    {934A9523-A3CA-4BC5-ADA0-D6D95D979421}
) do (
    reg delete "HKCU\Software\Classes\CLSID\%%G" /f >nul 2>&1
    reg delete "HKCU\Software\Classes\Wow6432Node\CLSID\%%G" /f >nul 2>&1
)

if exist "%INSTALL_DIR%" rmdir /S /Q "%INSTALL_DIR%"

echo Done. Vietcong will use the system DirectPlay again.
echo.
pause
