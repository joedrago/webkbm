@echo off
setlocal

set BUILD_TYPE=Debug
if /i "%1"=="release" set BUILD_TYPE=Release

echo Building webkbm [%BUILD_TYPE%]...

cmake -S . -B build -DCMAKE_BUILD_TYPE=%BUILD_TYPE% >nul 2>&1
if %errorlevel% neq 0 (
    echo CMake configure failed. Retrying with output:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    exit /b 1
)

cmake --build build --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)

copy /y "build\%BUILD_TYPE%\webkbm.exe" webkbm.exe >nul 2>&1
if not exist webkbm.exe (
    copy /y "build\webkbm.exe" webkbm.exe >nul 2>&1
)
if not exist webkbm.exe (
    echo ERROR: could not find built webkbm.exe
    exit /b 1
)

echo.
echo OK: webkbm.exe
