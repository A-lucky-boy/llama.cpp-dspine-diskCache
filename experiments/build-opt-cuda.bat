@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Build the opt tree with the same toolchain, architecture list and shared-lib
rem layout as build-cuda-shared.bat, so benchmark numbers are comparable and the
rem resulting output/ can be dropped into the deployment directory unchanged.
rem ---------------------------------------------------------------------------

set "SRC=C:\Users\Administrator\Desktop\experience\_agentComprehensive\llama.cpp-opt-opt-main"
set "BUILD=%SRC%\build-opt"
set "OUTPUT=%SRC%\output-opt"

set "VCToolsVersion=14.44.35207"
call "D:\Visual Studio\buildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo [ERROR] vcvars64 failed
    exit /b 1
)

cmake -S "%SRC%" -B "%BUILD%" ^
    -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER="D:/Visual Studio/buildTools/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64/cl.exe" ^
    -DCMAKE_CXX_COMPILER="D:/Visual Studio/buildTools/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64/cl.exe" ^
    -DCMAKE_CUDA_COMPILER="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/bin/nvcc.exe" ^
    -DCMAKE_CUDA_HOST_COMPILER="D:/Visual Studio/buildTools/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64/cl.exe" ^
    -DCMAKE_MAKE_PROGRAM="D:/Anaconda3/Scripts/ninja.exe" ^
    -DCMAKE_RC_COMPILER="D:/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
    -DCMAKE_MT="D:/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" ^
    -DCMAKE_CUDA_ARCHITECTURES=86-real;89-real ^
    -DGGML_CUDA=ON ^
    -DBUILD_SHARED_LIBS=ON ^
    -DLLAMA_CURL=OFF ^
    -DLLAMA_BUILD_TESTS=OFF ^
    -DGGML_NATIVE=OFF

if errorlevel 1 (
    echo [ERROR] CMake configure failed
    exit /b 1
)

cmake --build "%BUILD%" --config Release --parallel 16
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

if exist "%OUTPUT%" rmdir /s /q "%OUTPUT%"
mkdir "%OUTPUT%"
copy "%BUILD%\bin\*.exe" "%OUTPUT%\" >nul
copy "%BUILD%\bin\*.dll" "%OUTPUT%\" >nul
copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\cudart64_12.dll"     "%OUTPUT%\" >nul
copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\cublas64_12.dll"    "%OUTPUT%\" >nul
copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\cublasLt64_12.dll"  "%OUTPUT%\" >nul
copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvJitLink_120_0.dll" "%OUTPUT%\" >nul

echo.
echo === opt build staged to %OUTPUT% ===
exit /b 0
