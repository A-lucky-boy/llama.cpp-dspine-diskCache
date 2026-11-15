@echo off
setlocal

rem ---------------------------------------------------------------------------
rem Configure and build the CUDA shared build for RTX 4070 Ti SUPER (sm_89) and
rem RTX 3090 (sm_86).
rem
rem Binaries go straight to output\ because CMAKE_RUNTIME_OUTPUT_DIRECTORY points
rem there (see CMakeLists.txt); this script only adds the CUDA runtime DLLs that
rem do not come out of the build. output\ is git-ignored apart from .gitkeep.
rem ---------------------------------------------------------------------------

set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build-cuda-shared"
set "OUTPUT=%SRC%\output"

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

rem CUDA runtime DLLs are not produced by the build and must sit next to the exes
rem for a shared build; copy them only when missing so repeated builds stay cheap
if not exist "%OUTPUT%" mkdir "%OUTPUT%"
for %%D in (cudart64_12.dll cublas64_12.dll cublasLt64_12.dll nvJitLink_120_0.dll) do (
    if not exist "%OUTPUT%\%%D" (
        copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\%%D" "%OUTPUT%\" >nul
    )
)

echo.
echo === Build complete: %OUTPUT% ===
exit /b 0
