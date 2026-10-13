@echo off
setlocal
if not "%~1"=="" (
    echo Usage: build.cmd
    exit /b 2
)
set "LB_VS="
set "LB_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%LB_VSWHERE%" goto missing
for /f "usebackq tokens=*" %%i in (`call "%LB_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "LB_VS=%%i"
if not defined LB_VS for /f "usebackq tokens=*" %%i in (`call "%LB_VSWHERE%" -latest -version "[17.0,)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "LB_VS=%%i"
if not defined LB_VS goto missing
call "%LB_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "LB_CMAKE=%LB_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "LB_NINJA=%LB_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%LB_CMAKE%" goto missing
if not exist "%LB_NINJA%" goto missing

pushd "%~dp0"
"%LB_CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%LB_NINJA%"
if errorlevel 1 goto failed
"%LB_CMAKE%" --build build
if errorlevel 1 goto failed
"%LB_CMAKE%" --install build --prefix dist
if errorlevel 1 goto failed
echo Ready: %CD%\dist\LogiBattery.exe
popd
exit /b 0
:failed
popd
exit /b 1
:missing
echo Install Visual Studio 2022 or newer with Desktop development with C++ and C++ CMake tools.
exit /b 1
