@echo off
setlocal

rem ---------------------------------------------------------------------------
rem  ReShade Pre-UI add-on - local build script
rem
rem  Requires: Visual Studio 2022 (Desktop development with C++) + CMake >= 3.20
rem  Output:   build\Release\ReShade_PreUI.addon64
rem  Note:     the version number lives in CMakeLists.txt (project(... VERSION))
rem ---------------------------------------------------------------------------

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake not found in PATH.
    echo         Install "CMake Tools for Visual Studio" or CMake from cmake.org.
    exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build" -A x64
if errorlevel 1 goto :fail

cmake --build "%~dp0build" --config Release --parallel
if errorlevel 1 goto :fail

echo.
echo [OK] Output:
for %%f in ("%~dp0build\Release\*.addon64") do (
    echo      %%~ff
    echo      %%~zf bytes
)
echo.
echo [NEXT] Copy the .addon64 and PreUI.ini next to the game executable,
echo        the same folder where dxgi.dll / d3d12.dll (ReShade) lives.
exit /b 0

:fail
echo.
echo [ERROR] Build failed. See the compiler output above.
exit /b 1
