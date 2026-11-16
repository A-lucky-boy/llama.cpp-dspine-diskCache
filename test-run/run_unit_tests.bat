@echo off
REM Run every standalone disk-cache test.
REM
REM These are the tests that do not need a running server: formats, arithmetic and
REM filesystem behaviour. The server-level behaviour (cross-restart restore, media
REM identity, fault injection) is exercised by the python matrices under test-run\,
REM which need a live server and are not part of this script.
REM
REM Usage:  test-run\run_unit_tests.bat
REM
REM Each test prints "<something> passed" on success and exits non-zero on failure, so
REM the script stops at the first broken one rather than reporting a partial result.

setlocal
call "D:\Visual Studio\buildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."

set INC=/I tools\server /I common /I include /I ggml\include /I vendor
set FLAGS=/nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS
set OUT=%~dp0

echo === codec ===
cl %FLAGS% %INC% /Fe:%OUT%t_codec.exe tools\server\tests\unit\test_disk_cache_codec.cpp tools\server\server-disk-cache-codec.cpp || exit /b 1
%OUT%t_codec.exe || exit /b 1

echo === layout ===
cl %FLAGS% %INC% /Fe:%OUT%t_layout.exe tools\server\tests\unit\test_disk_cache_layout.cpp || exit /b 1
%OUT%t_layout.exe || exit /b 1

echo === lifecycle ===
cl %FLAGS% %INC% /Fe:%OUT%t_lifecycle.exe tools\server\tests\unit\test_disk_cache_lifecycle.cpp tools\server\server-disk-cache-quota.cpp tools\server\server-disk-cache-quarantine.cpp || exit /b 1
%OUT%t_lifecycle.exe || exit /b 1

echo === match ===
cl %FLAGS% %INC% /Fe:%OUT%t_match.exe tools\server\tests\unit\test_disk_cache_match.cpp tools\server\server-disk-cache-match.cpp || exit /b 1
%OUT%t_match.exe || exit /b 1

echo === quota ===
cl %FLAGS% %INC% /Fe:%OUT%t_quota.exe tools\server\tests\unit\test_disk_cache_quota.cpp tools\server\server-disk-cache-quota.cpp || exit /b 1
%OUT%t_quota.exe || exit /b 1

echo === prefix-index ===
cl %FLAGS% %INC% /Fe:%OUT%t_prefix.exe tools\server\tests\unit\test_cache_prefix_index.cpp tools\server\server-prefix-index.cpp || exit /b 1
%OUT%t_prefix.exe || exit /b 1

echo === materialize ===
cl %FLAGS% %INC% /Fe:%OUT%t_materialize.exe tools\server\tests\unit\test_prefix_materialize.cpp tools\server\server-prefix-materialize.cpp || exit /b 1
%OUT%t_materialize.exe || exit /b 1

echo === asan codec ===
cl /nologo /std:c++17 /EHsc /utf-8 /O1 /MDd /fsanitize=address /Zi /D_CRT_SECURE_NO_WARNINGS %INC% /Fe:%OUT%t_codec_asan.exe tools\server\tests\unit\test_disk_cache_codec.cpp tools\server\server-disk-cache-codec.cpp || exit /b 1
set ASAN_OPTIONS=detect_leaks=0
%OUT%t_codec_asan.exe || exit /b 1

echo === fs replacement semantics ===
cl %FLAGS% /Fe:%OUT%t_fsreplace.exe %OUT%fs_replace_probe.cpp || exit /b 1
%OUT%t_fsreplace.exe || exit /b 1

echo === media transition ===
rem links server-common.cpp, so it carries its own toolchain pinning and library set
call "%~dp0build_media_transition_test.bat" || exit /b 1

echo ALL PASSED
exit /b 0
