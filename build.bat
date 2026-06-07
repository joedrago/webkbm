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

:: Generate webkbm.reg pointing at the current absolute exe path.
:: Double-click it to install a per-user Run entry for autostart at login.
set "EXE_PATH=%CD%\webkbm.exe"
set "ESCAPED_PATH=%EXE_PATH:\=\\%"
> webkbm.reg (
    echo REGEDIT4
    echo.
    echo [HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run]
    echo "webkbm"="%ESCAPED_PATH%"
)

echo.
echo OK: webkbm.exe
echo OK: webkbm.reg ^(double-click to enable autostart^)
