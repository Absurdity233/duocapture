@echo off
rem 用 MSVC + Ninja 构建并跑测试。需要 Visual Studio 2022/2026 与 Windows SDK。
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. Install Visual Studio with C++ workload.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio C++ tools not found.
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

set "ROOT=%~dp0.."
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DDUO_BUILD_TESTS=ON
if errorlevel 1 exit /b 1

cmake --build "%ROOT%\build"
if errorlevel 1 exit /b 1

ctest --test-dir "%ROOT%\build" --output-on-failure
