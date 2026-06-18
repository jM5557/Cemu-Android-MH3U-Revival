@echo off
setlocal enabledelayedexpansion
set "VCPKG_ROOT=E:\Cemu-src\dependencies\vcpkg"
set "CM=D:\Program Files\Microsoft Visual Studio\2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d E:\Cemu-src

if not exist dependencies\vcpkg\vcpkg.exe (
  echo ===== bootstrapping vcpkg =====
  call dependencies\vcpkg\bootstrap-vcpkg.bat -disableMetrics
)

set /a tries=0
:configure
set /a tries+=1
echo ===== cmake configure attempt !tries! (resumes vcpkg cache) =====
"%CM%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=release -DVCPKG_INSTALL_OPTIONS=--clean-after-build
if errorlevel 1 (
  if !tries! lss 10 (
    echo configure failed - likely transient download, retrying in 5s...
    timeout /t 5 /nobreak >nul
    goto configure
  )
  echo CONFIGURE FAILED after !tries! attempts & exit /b 2
)

echo ===== building Cemu (release) =====
"%CM%" --build build --config release --parallel
if errorlevel 1 ( echo BUILD FAILED & exit /b 3 )

echo ===== BUILD DONE =====
dir /b build\bin\Cemu*.exe 2>nul
