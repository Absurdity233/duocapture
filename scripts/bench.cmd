@echo off
rem Build and run the jitter-buffer benchmark.
rem No dependency on build\duocore.lib: the few sources it needs are compiled
rem right here so the benchmark can use /O2 regardless of the project's build
rem type (mixing /MD with a Debug library fails the runtime check).
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

set "ROOT=%~dp0.."
cd /d "%ROOT%"

if not exist build mkdir build
if exist build\bench_obj rmdir /s /q build\bench_obj
mkdir build\bench_obj

rem /O2 + /MD: debug-runtime iterator checking (in /MDd builds) costs more
rem than the code under test, so the benchmark would measure the debug CRT.
rem Everything the benchmark needs is compiled here, so /MD is self-consistent.
rem In a Debug (/Od) build the 1200-byte payload copies dominate and hide
rem the cost of the FEC path entirely.
cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MD /Iinclude ^
  /Fo:build\bench_obj\ /Fd:build\bench_obj\ ^
  /c scripts\jitter_bench.cpp src\net\jitter_buffer.cpp src\net\fec.cpp ^
     src\net\packet.cpp src\net\packetizer.cpp src\common\clock.cpp src\common\log.cpp
if errorlevel 1 exit /b 1

cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MD /Iinclude ^
  /Fo:build\bench_obj\ /Fd:build\bench_obj\ ^
  build\bench_obj\jitter_bench.obj build\bench_obj\jitter_buffer.obj build\bench_obj\fec.obj ^
  build\bench_obj\packet.obj build\bench_obj\packetizer.obj build\bench_obj\clock.obj build\bench_obj\log.obj ^
  /link ws2_32.lib ole32.lib ^
  /out:build\jitter_bench.exe
if errorlevel 1 exit /b 1

build\jitter_bench.exe
