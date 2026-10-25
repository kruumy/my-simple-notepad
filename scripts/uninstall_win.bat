@echo off
setlocal

set "DEST=%LOCALAPPDATA%\Programs\simple_notepad"
set "SHORTCUT=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Simple Notepad.lnk"

reg delete "HKCU\Software\Classes\Applications\simple_notepad.exe" /f >nul 2>&1
reg delete "HKCU\Software\Classes\.txt\OpenWithList\simple_notepad.exe" /f >nul 2>&1
if exist "%SHORTCUT%" del "%SHORTCUT%"

if exist "%DEST%" (
    rmdir /s /q "%DEST%"
    if exist "%DEST%" (
        echo Could not remove "%DEST%". Close Simple Notepad if it is running and try again.
        exit /b 1
    )
)

if exist "%APPDATA%\simple_notepad.ini" del "%APPDATA%\simple_notepad.ini"
echo Uninstalled.
