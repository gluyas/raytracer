@echo off

set dxc_args=^
    -Zpr ^
    -D HLSL ^
    -I src\

@REM bluenoise.hlsl kernels
@REM generate_initial_sample_points
dxc %dxc_args% -T cs_6_3 ^
    -Fh out\bluenoise_generate_initial_sample_points.h /Vn g_generate_initial_sample_points_bytecode -E generate_initial_sample_points ^
    src/bluenoise.hlsl
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
@REM sort_initial_sample_points
dxc %dxc_args% -T cs_6_3 ^
    -Fh out\bluenoise_sort_initial_sample_points.h /Vn g_sort_initial_sample_points_bytecode -E sort_initial_sample_points ^
    src/bluenoise.hlsl
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
@REM build_hashtable
dxc %dxc_args% -T cs_6_3 ^
    -Fh out\bluenoise_build_hashtable.h /Vn g_build_hashtable_bytecode -E build_hashtable ^
    src/bluenoise.hlsl
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
@REM generate_sample_points
dxc %dxc_args% -T cs_6_3 ^
    -Fh out\bluenoise_generate_sample_points.h /Vn g_generate_sample_points_bytecode -E generate_sample_points ^
    src/bluenoise.hlsl
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
@REM generate single header
(
    echo #include ^"out\bluenoise_generate_initial_sample_points.h^"
    echo #include ^"out\bluenoise_sort_initial_sample_points.h^"
    echo #include ^"out\bluenoise_build_hashtable.h^"
    echo #include ^"out\bluenoise_generate_sample_points.h^"
) > out\bluenoise_hlsl.h
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )

@REM raytracing.hlsl DXIL library
dxc %dxc_args% -T lib_6_3 ^
    -Fh out\raytracing_hlsl.h /Vn g_raytracing_hlsl_bytecode ^
    src/raytracing.hlsl
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )

@REM main.cpp
cl ^
    -MP -nologo ^
    ^
    -Zi -EHsc ^
    -Foout\ -Fdout\ -Feout\raytracer ^
    ^
    -Ilib -Ilib\imgui -I. ^
    -DCPP -DUNICODE -DDEBUG^
    src\geometry.cpp src\parse_obj.cpp src\bluenoise.cpp src\device.cpp src\main.cpp ^
    ^
    out\lib.lib ^
    user32.lib ^
    d3d12.lib dxgi.lib d3dcompiler.lib ^
    ^
    -link -NOLOGO
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
