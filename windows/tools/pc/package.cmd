@echo off
rem Installs the built app into dist\FLX Film Lab, with its libraries, its
rem data and its licences, and builds the installer from it beside it
rem (tools\pc\installer.iss, with Inno Setup 7). tools\pc\remote.sh package
rem runs it, after a build.
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set "VS=%%i"
if not defined VS (echo error: no Visual Studio Build Tools & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0..\.."
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
if not exist "%ISCC%" (echo error: no Inno Setup 7 at %ISCC% & exit /b 1)
cmake --install "%ROOT%\build\win" --prefix "%ROOT%\dist\FLX Film Lab" > "%ROOT%\build\win-install.log" 2>&1 || (type "%ROOT%\build\win-install.log" & exit /b 1)
set /p VERSION=<"%ROOT%\build\win\version.txt"
"%ISCC%" /Q "/DAppVersion=%VERSION%" "%ROOT%\tools\pc\installer.iss" > "%ROOT%\build\win-installer.log" 2>&1 || (type "%ROOT%\build\win-installer.log" & exit /b 1)
