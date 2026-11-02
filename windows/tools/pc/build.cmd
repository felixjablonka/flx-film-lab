@echo off
rem Configures and builds the port on Windows with Clang (clang-cl) and Ninja
rem from the Visual Studio Build Tools, against Qt 6.12.0 in C:\Qt (QT_DIR
rem overrides it), as tools\pc\remote.sh runs it over SSH and the release
rem workflow on GitHub's Windows image.
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set "VS=%%i"
if not defined VS (echo error: no Visual Studio Build Tools & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0..\.."
if not defined QT_DIR set "QT_DIR=C:\Qt\6.12.0\msvc2022_64"
rem Visual Studio's own clang-cl, which the port is built and checked with,
rem not another LLVM on the PATH (GitHub's Windows images carry one).
set "CLANG_CL=%VS%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if exist "%CLANG_CL%" goto clang
echo error: no clang-cl in Visual Studio: install its C++ Clang tools
exit /b 1
:clang
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
cmake -S "%ROOT%" -B "%ROOT%\build\win" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=%CLANG_CL%" "-DCMAKE_PREFIX_PATH=%QT_DIR%" > "%ROOT%\build\win-configure.log" 2>&1 || (type "%ROOT%\build\win-configure.log" & exit /b 1)
cmake --build "%ROOT%\build\win" || exit /b 1
