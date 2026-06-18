@echo off
setlocal
set "VCPKG_ROOT=E:\Cemu-src\dependencies\vcpkg"
set "CM=D:\Program Files\Microsoft Visual Studio\2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d E:\Cemu-src

echo ===== [1/3] bootstrapping vcpkg =====
call dependencies\vcpkg\bootstrap-vcpkg.bat -disableMetrics
if errorlevel 1 ( echo BOOTSTRAP FAILED & exit /b 1 )

echo ===== [2/3] cmake configure (installs all deps via vcpkg - LONG) =====
"%CM%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=release -DVCPKG_INSTALL_OPTIONS=--clean-after-build
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 2 )

echo ===== [3/3] building Cemu (release) =====
"%CM%" --build build --config release --parallel
if errorlevel 1 ( echo BUILD FAILED & exit /b 3 )

echo ===== BUILD DONE =====
dir /b build\bin\Cemu*.exe 2>nul
