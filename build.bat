@echo off
echo Compiling Terminal Launcher...
g++ -O3 -std=c++17 main.cpp -o TerminalLauncher.exe -luser32 -lshell32 -lgdi32 -ladvapi32 -mwindows -static-libgcc -static-libstdc++ -static
if %ERRORLEVEL% NEQ 0 (
    echo Compilation FAILED!
    exit /b %ERRORLEVEL%
)
echo Compilation successful! Generated TerminalLauncher.exe
