@echo off
setlocal

rem MinGW-w64 (UCRT). No CMake, no IDE.
rem Link order matters: -lws2_32 must come after -liphlpapi.

if not exist build mkdir build

windres resources.rc -O coff -o build\resources.o
if errorlevel 1 (
  echo RESOURCE COMPILE FAILED
  exit /b 1
)

g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -Wall -Wextra ^
  src\main.cpp src\scene.cpp src\telemetry.cpp src\launcher.cpp src\audio.cpp src\media.cpp build\resources.o ^
  -o build\glance.exe ^
  -ld2d1 -ldwrite -ldwmapi -lwindowscodecs -lole32 -luuid -loleaut32 ^
  -lpdh -lpsapi -liphlpapi -lws2_32 -lgdi32 -luser32 -lshell32 ^
  -static-libgcc -static-libstdc++ -static ^
  -mwindows

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo BUILT build\glance.exe
