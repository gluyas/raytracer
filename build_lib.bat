@echo off

mkdir out


cl ^
    -MP -nologo ^
    ^
    -c -Zi ^
    -Foout\ -Fdout\ ^
    ^
    -Ilib\imgui ^
        lib\imgui\imgui.cpp lib\imgui\imgui_demo.cpp lib\imgui\imgui_draw.cpp lib\imgui\imgui_widgets.cpp ^
        lib\imgui\imgui_impl_win32.cpp lib\imgui\imgui_impl_dx12.cpp ^
    ^
    -link -NOLOGO

lib -nologo -OUT:out\lib.lib ^
    out\imgui.obj out\imgui_demo.obj out\imgui_draw.obj out\imgui_widgets.obj out\imgui_impl_win32.obj out\imgui_impl_dx12.obj 
