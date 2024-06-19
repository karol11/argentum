@pushd "%~dp0..\workdir"
@if exist "..\apps\%~1.obj" del "..\apps\%~1.obj"
@"%~dp0agc" -src "..\ag" -start %1 -O3 -o "..\apps\%~1.obj"
@IF '%ERRORLEVEL%'=='0' "%~dp0lld-link" /out:"..\apps\%~1.exe" /libpath:"..\libs" "..\apps\%~1.obj" "..\libs\ag_runtime.lib" ^
   "..\libs\ag_sdl.lib" "..\libs\SDL2.lib" "..\libs\SDL2_image.lib" "..\libs\SDL2_ttf.lib" ^
   "..\libs\ag_http_client.lib" "..\libs\libcurl.lib" ^
   "..\libs\ag_sqlite.lib" "..\libs\sqlite3.lib"
@IF '%ERRORLEVEL%'=='0' "..\apps\%~1.exe"
@popd
