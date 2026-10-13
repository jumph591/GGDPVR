@echo off
rem Builds dinput8.dll (proxy) and GGDPVR.dll with MSVC v143 + Ninja into out\%CONFIG%.
setlocal
for /f "delims=" %%p in ('where python 2^>nul') do if not defined PYEXE set "PYEXE=%%p"
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo
set VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%SystemRoot%\system32;%SystemRoot%
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_MAKE_PROGRAM=ninja >nul || exit /b 1
rem the UI layer shader is compiled at runtime: check it here (a failed compile disables the whole composite)
if defined PYEXE ("%PYEXE%" "%~dp0tools\check_shader.py" "%~dp0src\vr\ui_layer.cpp" || (echo error: UI layer shader does not compile & exit /b 1)) else (echo warning: python not found, UI shader not checked)
cmake --build "%~dp0build\%CONFIG%" || exit /b 1
if not exist "%~dp0out\%CONFIG%" mkdir "%~dp0out\%CONFIG%"
for %%f in (dinput8.dll dinput8.pdb GGDPVR.dll GGDPVR.pdb GGDPVR-Setup.exe) do copy /y "%~dp0build\%CONFIG%\%%f" "%~dp0out\%CONFIG%\" >nul
dir /b "%~dp0out\%CONFIG%"
