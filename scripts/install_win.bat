@echo off
setlocal

rem installs for the current user only, so no admin rights are needed
set "DEST=%LOCALAPPDATA%\Programs\simple_notepad"
set "EXE=%DEST%\simple_notepad.exe"
set "SHORTCUT=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Simple Notepad.lnk"
set "APP_KEY=HKCU\Software\Classes\Applications\simple_notepad.exe"

rem the build copies this script beside the exe
set "SRC=%~dp0simple_notepad.exe"
if not exist "%SRC%" (
    echo simple_notepad.exe not found next to this script. Run the copy in build\Release or build\Debug.
    exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
copy /y "%SRC%" "%EXE%" >nul || (
    echo Could not copy to "%DEST%". Close Simple Notepad if it is running and try again.
    exit /b 1
)

powershell -NoProfile -Command "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:SHORTCUT); $s.TargetPath = $env:EXE; $s.WorkingDirectory = $env:DEST; $s.Save()" || (
    echo Could not create the Start Menu shortcut.
    exit /b 1
)

rem "open with" entry for text files
reg add "%APP_KEY%" /v FriendlyAppName /d "Simple Notepad" /f >nul
reg add "%APP_KEY%\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul
reg add "%APP_KEY%\SupportedTypes" /v .txt /d "" /f >nul
reg add "HKCU\Software\Classes\.txt\OpenWithList\simple_notepad.exe" /f >nul

echo Installed to "%DEST%".
