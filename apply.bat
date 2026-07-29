@echo off
REM==============================================================================
REM apply.bat -- merge tuya-agentic integration code into the JieLi AC79 SDK
REM
REM Usage:
REM   apply.bat <AC79_SDK_root>
REM Example:
REM   apply.bat ..\fw-AC79_AIoT_SDK
REM   apply.bat .                 (run from inside the SDK root)
REM
REM Copies everything under overlay\ (preserving SDK-relative paths) into the
REM SDK root. Only for official SDK tag: AC79NN_SDK_V1.2.0
REM==============================================================================
setlocal

if "%~1"=="" (
  echo Usage: apply.bat ^<AC79_SDK_root^>
  echo Example: apply.bat ..\fw-AC79_AIoT_SDK
  exit /b 1
)

set "SDK=%~1"
set "SDK=%SDK:/=\%"

if not exist "%SDK%\apps\" (
  echo ERROR: "%SDK%" has no apps\ folder. Is this the AC79 SDK root?
  exit /b 1
)
if not exist "%SDK%\cpu\wl82\" (
  echo ERROR: "%SDK%" has no cpu\wl82\. Is this the AC79 SDK root?
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "OVER=%SCRIPT_DIR%overlay"

if not exist "%OVER%\" (
  echo ERROR: overlay\ not found at "%OVER%"
  exit /b 1
)

echo ==^> SDK root : %SDK%
echo ==^> overlay  : %OVER%
echo ==^> Merging ^(overwriting existing files^)...
xcopy "%OVER%\*" "%SDK%\" /e /y /i /q
if errorlevel 1 (
  echo ERROR: xcopy failed.
  exit /b 1
)
echo ==^> Done.
echo.
echo Next, build:
echo     cd "%SDK%"
echo     make ac791n_wifi_story_machine
echo.
echo To review changes instead of overwriting, use the patch:
echo     cd "%SDK%" ^&^& git apply "%SCRIPT_DIR%patches\tuya-agentic-v1.2.0.patch"

endlocal
