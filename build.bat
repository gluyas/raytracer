@echo off

dxc ^
    -T lib_6_3 -Zpr ^
    -Fh out\raytracing.hlsl.h /Vn raytracing_hlsl_bytecode ^
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
    -DUNICODE -DDEBUG ^
    src\main.cpp ^
    ^
    out\lib.lib ^
    user32.lib ^
    d3d12.lib dxgi.lib d3dcompiler.lib
