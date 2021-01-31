@echo off
setlocal

call ./build
if %ERRORLEVEL% neq 0 (
    exit /b %ERRORLEVEL%
)

echo.
.\out\raytracer %*

endlocal
