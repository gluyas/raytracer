@echo off

cl ^
    /Foout\ /Fdout\ /Feout\raytracer ^
    ^
    /EHsc ^
    ^
    /Ilib /Ilib\imgui /Ilib\DirectXMath ^
    /DUNICODE ^
    src\main.cpp ^
    ^
    out\lib.lib ^
    user32.lib ^
    d3d12.lib dxgi.lib d3dcompiler.lib ^
    %*
