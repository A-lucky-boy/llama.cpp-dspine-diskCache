@echo off
rem Build and run the media-transition unit test.
rem
rem The toolchain is pinned to the same MSVC the main build uses (14.44.35207).
rem The default vcvars64 here selects a different install whose runtime libraries are
rem not on the library path, which shows up as a missing msvcprt.lib at link time.
setlocal
set "VCROOT=D:\Visual Studio\buildTools\VC\Tools\MSVC\14.44.35207"
call "D:\Visual Studio\buildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."

set "PATH=%VCROOT%\bin\HostX64\x64;%PATH%"
set "LIB=%VCROOT%\lib\x64;%LIB%"
set "INCLUDE=%VCROOT%\include;%INCLUDE%"

cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
   /I tools\server /I tools\mtmd /I common /I include /I ggml\include /I vendor ^
   /Fe:test-run\t_media_transition.exe ^
   tools\server\tests\unit\test_cache_media_transition.cpp ^
   tools\server\server-common.cpp ^
   build-cuda-shared\common\llama-common.lib ^
   build-cuda-shared\src\llama.lib ^
   build-cuda-shared\tools\mtmd\mtmd.lib ^
   build-cuda-shared\ggml\src\ggml.lib ^
   build-cuda-shared\ggml\src\ggml-base.lib ^
   build-cuda-shared\ggml\src\ggml-cpu.lib
if errorlevel 1 exit /b 1

set "PATH=%CD%\output;%PATH%"
test-run\t_media_transition.exe
exit /b %errorlevel%
