@cd /d "%~dp0..\workdir"
@if exist "%~1.obj" del "%~1.obj"
@"%~dp0agc" -src "..\src" -start %1 -o "%~1.obj"
@IF '%ERRORLEVEL%'=='0' "%~dp0lld-link" /libpath:"..\libs" "%~1.obj" "..\libs\ag_runtime.lib" "..\libs\SDL2.lib" "..\libs\SDL2_image.lib"
@IF '%ERRORLEVEL%'=='0' "%~1.exe"
