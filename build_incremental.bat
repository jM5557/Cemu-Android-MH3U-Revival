@echo off
set "CM=D:\Program Files\Microsoft Visual Studio\2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d E:\Cemu-src
echo ===== incremental build (release) =====
"%CM%" --build build --config release --parallel
if errorlevel 1 ( echo BUILD FAILED & exit /b 3 )
echo ===== BUILD DONE =====
dir /b bin\Cemu*.exe
