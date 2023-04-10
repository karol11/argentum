del "%~1.obj"
"%~dp0agc" -src "%~dp0..\src" -start %1 -o "%~1.obj"
IF '%ERRORLEVEL%'=='0' "%~dp0lld-link" "%~1.obj" /libpath:"%~dp0..\libs" "%~dpn1.obj" "%~dp0..\libs\ag_runtime.lib" "%~dp0..\libs\SDL2.lib" "%~dp0..\libs\SDL2_image.lib"
IF '%ERRORLEVEL%'=='0' "%~dpn1.exe"
