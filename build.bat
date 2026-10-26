@echo off
setlocal

rem MinGW-w64 (UCRT). No CMake, no IDE.
rem Link order matters: -lws2_32 must come after -liphlpapi.

if not exist build mkdir build

rem Vendored SQLite (public domain) reads the Windows notification store.
rem Built once; SQLITE_THREADSAFE=0 because the panel is single-threaded.
if not exist build\sqlite3.o (
  gcc -c -O2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 ^
    -DSQLITE_OMIT_DEPRECATED -Ithird_party third_party\sqlite3.c -o build\sqlite3.o
  if errorlevel 1 (
    echo SQLITE COMPILE FAILED
    exit /b 1
  )
)

windres resources.rc -O coff -o build\resources.o
if errorlevel 1 (
  echo RESOURCE COMPILE FAILED
  exit /b 1
)

g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -Wall -Wextra -Ithird_party ^
  src\main.cpp src\scene.cpp src\telemetry.cpp src\launcher.cpp src\audio.cpp ^
  src\media.cpp src\ambient.cpp src\notifs.cpp src\activate.cpp src\settings.cpp ^
  build\sqlite3.o build\resources.o ^
  -o build\glance.exe ^
  -ld2d1 -ldwrite -ldwmapi -lwindowscodecs -lole32 -luuid -loleaut32 ^
  -lpdh -lpsapi -liphlpapi -lws2_32 -lgdi32 -luser32 -lshell32 -lshlwapi ^
  -lstdc++ ^
  -static-libgcc -static-libstdc++ -static ^
  -mwindows

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo BUILT build\glance.exe
