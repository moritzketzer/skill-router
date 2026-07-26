@echo off
setlocal

if not exist build mkdir build

REM -DSQLITE_ENABLE_FTS5 enables the full-text index used by hybrid search
REM (exact + FTS5 stemmed + fuzzy). Without it the engine still builds and runs,
REM degrading gracefully to exact + fuzzy search.
cl /nologo /O2 /MT /c /TC /DSQLITE_THREADSAFE=1 /DSQLITE_ENABLE_FTS5 third_party\sqlite3.c /Fobuild\sqlite3.obj
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /I. main.cpp build\sqlite3.obj ws2_32.lib /Fe:build\skillrouter_msvc.exe
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /I. test_library.cpp build\sqlite3.obj /Fe:build\test_library.exe
if errorlevel 1 exit /b %errorlevel%

REM Publish the freshly built binary as the package bin (package.json -> ./skillrouter.exe).
copy /Y build\skillrouter_msvc.exe skillrouter.exe >nul
if errorlevel 1 exit /b %errorlevel%

echo Build OK: skillrouter.exe updated.
