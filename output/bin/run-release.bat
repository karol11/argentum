@pushd "%~dp0..\workdir"
@if exist "%~dp0lld-link.exe" (
   set linker="%~dp0lld-link.exe" /debug /libpath:"..\libs" /out:
) else (
   set linker="cl.exe" /Fe
)
@if exist "..\apps\%~1.obj" del "..\apps\%~1.obj"
@"%~dp0agc" -src "..\ag" -start %1 -O3 -o "..\apps\%~1.obj" -g
@IF '%ERRORLEVEL%'=='0' %linker%"..\apps\%~1.exe" "..\apps\%~1.obj" "..\libs\ag_runtime.lib" ^
   "..\libs\ag_sdl.lib" "..\libs\SDL2.lib" "..\libs\SDL2_image.lib" "..\libs\SDL2_ttf.lib" ^
   "..\libs\ag_http_client.lib" "..\libs\libcurl.lib" ^
   "..\libs\ag_sqlite.lib" "..\libs\sqlite3.lib"
@IF '%ERRORLEVEL%'=='0' "..\apps\%~1.exe"
@popd
