@cd /d "%~dp0..\debug"
@if exist "..\apps\%~1.obj" del "..\apps\%~1.obj"
"%~dp0agc" -g -src "..\ag" -start %1 -O0 -o "..\apps\%~1.obj"
@IF '%ERRORLEVEL%'=='0' "%~dp0lld-link" /out:"..\apps\%~1.exe" /debug /libpath:"..\libs" "..\apps\%~1.obj" "libs\ag_runtime.lib" ^
   "libs\ag_sdl.lib" "libs\SDL2d.lib" "libs\SDL2_imaged.lib" "libs\SDL2_ttfd.lib" ^
   "libs\ag_http_client.lib" "libs\libcurl-d.lib" ^
   "libs\ag_sqlite.lib" "libs\sqlite3.lib" ^
   "libs\gui_platform.lib" "libs\skia.dll.lib"
