@echo off
setlocal enabledelayedexpansion

set "demos=helloWorld sceneDemo sqliteDemo bottles threadTest graph httpDemo demo"

for %%n in (%demos%) do (
    echo [33mLaunching %%n.ag:
    echo [0m
    type ag\%%n.ag
    echo [32m
    pause
    echo [0m
    call "%~dp0bin\run-release.bat" %%n
    echo [32m
    pause
    echo [0m
    cls
)

cls
echo All compiled examples:
echo bytes [32mName[0m
for %%a in (apps\*.exe) do (
    set /a "size=%%~za/1024"
    echo !size!K   workdir/[32m%%~na[0m.exe
)
echo [0m
echo For more info see [32mREADME.md[0m
echo [32m
pause
echo [0m
endlocal
