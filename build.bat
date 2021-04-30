@echo off

dxc ^
    -T lib_6_3 -Zpr ^
    -Fh out\bluenoise.hlsl.h /Vn g_bluenoise_hlsl_bytecode ^
    ^
    -I src\ ^
    -D HLSL ^
    src/bluenoise.hlsl

if %ERRORLEVEL% neq 0 (
    exit /b %ERRORLEVEL%
)

dxc ^
    -T lib_6_3 -Zpr ^
    -Fh out\raytracing.hlsl.h /Vn g_raytracing_hlsl_bytecode ^
    ^
    -I src\ ^
    -D HLSL ^
    src/raytracing.hlsl

if %ERRORLEVEL% neq 0 (
    exit /b %ERRORLEVEL%
)

cl ^
    -Zi -EHsc ^
    -Foout\ -Fdout\ -Feout\raytracer ^
    ^
    -Ilib -Ilib\imgui -Ilib\DirectXMath -I. ^
    -DCPP -DUNICODE -DDEBUG ^
    src\parse_obj.cpp src\bluenoise.cpp src\main.cpp ^
    ^
    out\lib.lib ^
    user32.lib ^
    d3d12.lib dxgi.lib d3dcompiler.lib
