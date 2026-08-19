@echo off
setlocal EnableDelayedExpansion
rem ---------------------------------------------------------------------------
rem  Build NVR from src and deploy it to the game, clearing the shader cache.
rem
rem  Deploying shaders without the DLL, or the reverse, leaves a mismatched pair
rem  that fails silently rather than erroring, so this always does both.
rem
rem  Usage:  deploy.bat            build + deploy
rem          deploy.bat shaders    shaders and toml only, skip the build
rem          deploy.bat clean      full rebuild, then deploy
rem ---------------------------------------------------------------------------

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

rem Game folder. Override by setting FNV_PATH before calling.
if not defined FNV_PATH set "FNV_PATH=C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas"

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
set "TARGET=Build"
set "DOBUILD=1"

if /i "%~1"=="clean"   set "TARGET=Rebuild"
if /i "%~1"=="shaders" set "DOBUILD=0"

set "PLUGINS=%FNV_PATH%\Data\NVSE\plugins"
set "SHADERS=%FNV_PATH%\Data\Shaders\NewVegasReloaded"
set "DLLSRC=%REPO%\NewVegasReloaded\Release\NewVegasReloaded.dll"

echo.
echo  repo : %REPO%
echo  game : %FNV_PATH%
echo.

rem --- sanity ----------------------------------------------------------------
if not exist "%FNV_PATH%\FalloutNV.exe" (
    echo  [ERROR] No FalloutNV.exe under "%FNV_PATH%".
    echo          Set FNV_PATH to the game folder and retry.
    exit /b 1
)
if not exist "%PLUGINS%" (
    echo  [ERROR] Missing "%PLUGINS%" -- is NVSE installed?
    exit /b 1
)

rem The DLL stays locked while the game runs, and a half-finished deploy is
rem worse than none, so refuse up front rather than failing midway.
tasklist /fi "imagename eq FalloutNV.exe" 2>nul | find /i "FalloutNV.exe" >nul
if not errorlevel 1 (
    echo  [ERROR] Fallout New Vegas is running. Close it first.
    exit /b 1
)

rem --- build -----------------------------------------------------------------
if "%DOBUILD%"=="1" (
    if not exist "%MSBUILD%" (
        echo  [ERROR] MSBuild not found at:
        echo          %MSBUILD%
        echo          Edit MSBUILD in this script to match your VS install.
        exit /b 1
    )
    echo  [1/4] %TARGET% Release^|x86 . . .
    "%MSBUILD%" "%REPO%\TESReloaded.sln" -t:%TARGET% -p:Configuration=Release ^
        -p:Platform=x86 -p:PlatformToolset=v143 -v:minimal -nologo -m
    if errorlevel 1 (
        echo.
        echo  [ERROR] Build failed. Nothing deployed.
        exit /b 1
    )
    if not exist "%DLLSRC%" (
        echo  [ERROR] Build reported success but "%DLLSRC%" is missing.
        exit /b 1
    )
) else (
    echo  [1/4] skipping build ^(shaders only^)
)

rem --- plugin ----------------------------------------------------------------
echo  [2/4] plugin . . .
if "%DOBUILD%"=="1" (
    copy /y "%DLLSRC%" "%PLUGINS%\NewVegasReloaded.dll" >nul
    if errorlevel 1 echo  [ERROR] Could not copy the DLL. & exit /b 1
    for %%F in ("%PLUGINS%\NewVegasReloaded.dll") do echo        dll   %%~zF bytes
)
copy /y "%REPO%\resource\NewVegasReloaded.dll.defaults.toml" "%PLUGINS%\" >nul
if errorlevel 1 echo  [ERROR] Could not copy defaults.toml. & exit /b 1
echo        defaults.toml

rem --- shaders ---------------------------------------------------------------
rem /njh /njs /ndl /np quiet; /e recurses. Robocopy exit codes below 8 are
rem success, so anything under 8 has to be swallowed or the script aborts.
echo  [3/4] shaders and effects . . .
robocopy "%REPO%\src\hlsl\NewVegas\Shaders" "%SHADERS%\Shaders" /e /njh /njs /ndl /np >nul
if errorlevel 8 ( echo  [ERROR] robocopy failed on Shaders. & exit /b 1 )
robocopy "%REPO%\src\hlsl\NewVegas\Effects" "%SHADERS%\Effects" /e /njh /njs /ndl /np >nul
if errorlevel 8 ( echo  [ERROR] robocopy failed on Effects. & exit /b 1 )
if exist "%REPO%\resource\Textures" (
    robocopy "%REPO%\resource\Textures" "%FNV_PATH%\Data\Textures" /e /njh /njs /ndl /np >nul
    if errorlevel 8 ( echo  [ERROR] robocopy failed on Textures. & exit /b 1 )
)
echo        ok

rem --- cache -----------------------------------------------------------------
rem NVR keys the cache on preprocessed source and normally invalidates itself,
rem but a stale entry silently costs a whole test cycle, so clear it outright.
echo  [4/4] shader cache . . .
set "CLEARED=0"
if exist "%SHADERS%\Shaders\Cache" (
    rd /s /q "%SHADERS%\Shaders\Cache"
    set "CLEARED=1"
)
if exist "%SHADERS%\Effects\Cache" (
    rd /s /q "%SHADERS%\Effects\Cache"
    set "CLEARED=1"
)
if "!CLEARED!"=="1" (echo        cleared -- first launch recompiles) else (echo        none present)

echo.
echo  Done. Deployed from %REPO%
for /f "delims=" %%B in ('git -C "%REPO%" rev-parse --abbrev-ref HEAD 2^>nul') do echo  Branch: %%B
for /f "delims=" %%C in ('git -C "%REPO%" log --oneline -1 2^>nul') do echo  Commit: %%C
echo.
endlocal
