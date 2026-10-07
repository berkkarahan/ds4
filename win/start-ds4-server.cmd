@echo off
setlocal EnableExtensions

rem Prepend the ROCm runtime to PATH, then start the repo-root ds4-server.exe.
rem Serves OpenAI-style chat completions at http://HOST:PORT/v1/chat/completions.
rem Extra arguments are appended and override the value flags below.
rem ROCM_PATH overrides the SDK root. Optional overrides:
rem   DS4_MODEL, DS4_HOST, DS4_PORT, DS4_CTX, DS4_CACHE_EXPERTS
if "%ROCM_PATH%"=="" set "ROCM_PATH=C:\Program Files\AMD\ROCm\7.2"
if not exist "%ROCM_PATH%\bin\" (
    echo ROCm bin not found: "%ROCM_PATH%\bin">&2
    exit /b 1
)
set "PATH=%ROCM_PATH%\bin;%PATH%"
if not defined HIP_VISIBLE_DEVICES set "HIP_VISIBLE_DEVICES=0"

cd /d "%~dp0.."
if not exist "%~dp0..\ds4-server.exe" (
    echo Missing ds4-server.exe. Build with: make windows-rocm>&2
    exit /b 1
)

if /I "%~1"=="--help" goto :run
if /I "%~1"=="-h" goto :run

if not defined DS4_MODEL set "DS4_MODEL=ds4flash.gguf"
if not defined DS4_HOST set "DS4_HOST=127.0.0.1"
if not defined DS4_PORT set "DS4_PORT=18080"
if not defined DS4_CTX set "DS4_CTX=32768"
if not defined DS4_CACHE_EXPERTS set "DS4_CACHE_EXPERTS=512"

if not exist "%DS4_MODEL%" (
    echo Missing model: "%DS4_MODEL%">&2
    echo Set DS4_MODEL to a GGUF path, or link the Q2 file as ds4flash.gguf.>&2
    exit /b 1
)

echo ds4 chat completions: http://%DS4_HOST%:%DS4_PORT%/v1/chat/completions
echo model=%DS4_MODEL% ctx=%DS4_CTX% experts=%DS4_CACHE_EXPERTS% gpu=%HIP_VISIBLE_DEVICES%
if not "%~1"=="" echo extra arguments override earlier flags

"%~dp0..\ds4-server.exe" --rocm -m "%DS4_MODEL%" --host %DS4_HOST% --port %DS4_PORT% -c %DS4_CTX% --ssd-streaming --ssd-streaming-cache-experts %DS4_CACHE_EXPERTS% %*
exit /b %ERRORLEVEL%

:run
"%~dp0..\ds4-server.exe" %*
exit /b %ERRORLEVEL%
