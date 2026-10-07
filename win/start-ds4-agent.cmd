@echo off
setlocal EnableExtensions

rem Prepend the ROCm runtime to PATH, then start the repo-root ds4-agent.exe.
rem Extra arguments are forwarded unchanged. ROCM_PATH overrides the SDK root.
if "%ROCM_PATH%"=="" set "ROCM_PATH=C:\Program Files\AMD\ROCm\7.2"
if not exist "%ROCM_PATH%\bin\" (
    echo ROCm bin not found: "%ROCM_PATH%\bin">&2
    exit /b 1
)
set "PATH=%ROCM_PATH%\bin;%PATH%"
if not defined HIP_VISIBLE_DEVICES set "HIP_VISIBLE_DEVICES=0"

cd /d "%~dp0.."
if not exist "%~dp0..\ds4-agent.exe" (
    echo Missing ds4-agent.exe. Build with: make windows-rocm>&2
    exit /b 1
)
"%~dp0..\ds4-agent.exe" %*
exit /b %ERRORLEVEL%
