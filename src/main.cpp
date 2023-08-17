#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "prelude.h"

#include "device.h"
using Device::g_device;
using Device::g_descriptor_size;
using Device::g_rtv_descriptor_size;

#include "parse_obj.h"
#include "raytracing.h"
#include "bluenoise.h"

#define SCENE_NAME "final"

#define WINDOW_STYLE_EX WS_EX_OVERLAPPEDWINDOW
#define WINDOW_STYLE (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
#define WINDOW_MENU 0

#define MIN_RESOLUTION 256

#define VSYNC 0
#define SWAPCHAIN_BUFFER_COUNT 2

// TODO: move to same header as HLSL root signature definition
#define IMGUI_DESCRIPTOR_INDEX 0
#define RAYTRACING_DESCRIPTOR_INDEX 2
#define DESCRIPTORS_COUNT 1024

// GLOBAL STATE

HWND g_hwnd = NULL;

UINT  g_width;
UINT  g_height;
float g_aspect;

ID3D12CommandQueue*     g_cmd_queue     = NULL;
ID3D12CommandAllocator* g_cmd_allocator = NULL;

IDXGISwapChain4* g_swapchain = NULL;
ID3D12DescriptorHeap* g_rtv_descriptor_heap = NULL;
ID3D12Resource* g_rtvs[SWAPCHAIN_BUFFER_COUNT] = {};
Fence g_fence;

ID3D12DescriptorHeap* g_descriptor_heap = NULL;

bool g_do_update_resolution = true;
bool g_do_reset_accumulator = true;
bool g_do_reset_translucent_accumulator = true;
float g_do_regenerate_translucent_samples = 0.025;
float g_sample_points_radius = 0;
UINT g_total_sample_points = 0;
UINT g_prevent_resizing = 0;

// UTILITY FUNCTIONS

void update_resolution() {
    { // get client area
        RECT rect;
        GetClientRect(g_hwnd, &rect);
        g_width  = rect.right;
        g_height = rect.bottom;
        g_aspect = (float) g_width / (float) g_height;
        g_do_reset_accumulator = true;
    }

    if (!g_swapchain) {
        DXGI_SWAP_CHAIN_DESC1 g_swapchain_desc = {};
        g_swapchain_desc.Format             = PIXEL_FORMAT;
        g_swapchain_desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        g_swapchain_desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        g_swapchain_desc.BufferCount        = SWAPCHAIN_BUFFER_COUNT;
        g_swapchain_desc.SampleDesc.Count   = 1;
        g_swapchain_desc.SampleDesc.Quality = 0;
        g_swapchain_desc.Scaling            = DXGI_SCALING_NONE;
        g_swapchain_desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;

        IDXGISwapChain1* swapchain1 = NULL;
        CHECK_RESULT(Device::g_dxgi_factory->CreateSwapChainForHwnd(
            g_cmd_queue, g_hwnd,
            &g_swapchain_desc, NULL, NULL,
            &swapchain1
        ));
        CHECK_RESULT(swapchain1->QueryInterface(IID_PPV_ARGS(&g_swapchain)));
        swapchain1->Release(); // QueryInterface increments ref count
    } else {
        // release rtv references
        for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
            g_rtvs[i]->Release();
        }
        CHECK_RESULT(g_swapchain->ResizeBuffers(SWAPCHAIN_BUFFER_COUNT, g_width, g_height, PIXEL_FORMAT, 0));
    }

    { // g_rtvs
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
            CHECK_RESULT(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_rtvs[i])));
            g_device->CreateRenderTargetView(g_rtvs[i], NULL, rtv_handle);
            SET_NAME(g_rtvs[i]);
            rtv_handle.ptr += g_rtv_descriptor_size;
        }
    }

    Raytracing::update_resolution(g_width, g_height);
    Raytracing::update_descriptors({ g_descriptor_heap, RAYTRACING_DESCRIPTOR_INDEX });
}

LRESULT CALLBACK WindowProc(
    HWND   g_hwnd,
    UINT   msg,
    WPARAM wp,
    LPARAM lp
) {
    if (ImGui_ImplWin32_WndProcHandler(g_hwnd, msg, wp, lp)) return 0;

    switch (msg) {
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        } break;

        case WM_SIZE: {
            // TODO: verify thread safety
            g_do_update_resolution = true;
            return 0;
        } break;

        case WM_GETMINMAXINFO: {
            RECT rect = {};
            rect.right  = MIN_RESOLUTION;
            rect.bottom = MIN_RESOLUTION;
            AdjustWindowRectEx(&rect, WINDOW_STYLE, WINDOW_MENU, WINDOW_STYLE_EX);

            LPMINMAXINFO info = (LPMINMAXINFO) lp;
            info->ptMinTrackSize.x = rect.right;
            info->ptMinTrackSize.y = rect.bottom;
        }

        default: {
            return DefWindowProc(g_hwnd, msg, wp, lp);
        } break;
    }
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    PWSTR     pCmdLine,
    int       nCmdShow
) {
    // PLATFORM LAYER
    { // g_hwnd
        WNDCLASS wc = {};
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = L"RaytracerWindowClass";

        RECT rect = {};
        // rect.right  = 1600;
        // rect.bottom = 900;
        rect.right  = 1000;
        rect.bottom = 955;
        AdjustWindowRectEx(&rect, WINDOW_STYLE, WINDOW_MENU, WINDOW_STYLE_EX);

        RegisterClass(&wc);
        g_hwnd = CreateWindowExW(
            WINDOW_STYLE_EX,
            wc.lpszClassName, L"Raytracer",
            WINDOW_STYLE,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right-rect.left, rect.bottom-rect.top,
            NULL, WINDOW_MENU, hInstance, NULL
        );
    }

    srand(GetTickCount64());

    // INITIALIZE MODULES

    Device::init();

    { // g_cmd_queue
        D3D12_COMMAND_QUEUE_DESC g_cmd_queue_desc = {};
        g_cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        g_cmd_queue_desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;

        CHECK_RESULT(g_device->CreateCommandQueue(&g_cmd_queue_desc, IID_PPV_ARGS(&g_cmd_queue)));
    }

    // g_cmd_allocator
    CHECK_RESULT(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_allocator)));

    { // g_rtv_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_rtv_descriptor_heap_desc = {};
        g_rtv_descriptor_heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        g_rtv_descriptor_heap_desc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;

        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_rtv_descriptor_heap_desc, IID_PPV_ARGS(&g_rtv_descriptor_heap)));
    }

    // g_fence
    g_fence = Fence::make();

    { // g_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_descriptor_heap_desc = {};
        g_descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        g_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_descriptor_heap_desc.NumDescriptors = DESCRIPTORS_COUNT;
        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_descriptor_heap_desc, IID_PPV_ARGS(&g_descriptor_heap)));
    }

    ID3D12GraphicsCommandList4* cmd_list = NULL;
    CHECK_RESULT(g_device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmd_list)));
    CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));

    Bluenoise::init();
    Raytracing::init(cmd_list);

    // SETUP

    update_resolution();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_Init(
        g_device, SWAPCHAIN_BUFFER_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        g_descriptor_heap,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), IMGUI_DESCRIPTOR_INDEX, g_descriptor_size),
        CD3DX12_GPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetGPUDescriptorHandleForHeapStart(), IMGUI_DESCRIPTOR_INDEX, g_descriptor_size)
    );

    // SCENE CREATION
    Array<BlasInstance> instances = {};

    Aabb cornell_aabb = AABB_NULL;
    Blas cornell_blas; {
        Array<GeometryInstance> geometries = {};
        Array<Array<Vertex>> all_vertices = {};
        Array<Array<Index>>  all_indices  = {};

        { // white walls
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};
            parse_obj_file("data/cornell/floor.obj",   true, &vertices, &indices, &cornell_aabb);
            parse_obj_file("data/cornell/back.obj",    true, &vertices, &indices, &cornell_aabb);
            parse_obj_file("data/cornell/ceiling.obj", true, &vertices, &indices, &cornell_aabb);

            GeometryInstance geometry = {};
            geometry.material.color  = { 1.0, 1.0, 1.0 };
            geometry.material.shader = Shader::Lambert;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }
        { // red wall
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};

            GeometryInstance geometry = {};
            parse_obj_file("data/cornell/redwall.obj", true, &vertices, &indices, &cornell_aabb);
            geometry.material.color  = { 1.0, 0.0, 0.0 };
            geometry.material.shader = Shader::Lambert;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }
        { // green wall
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};

            parse_obj_file("data/cornell/greenwall.obj", true, &vertices, &indices, &cornell_aabb);
            GeometryInstance geometry = {};
            geometry.material.color  = { 0.0, 1.0, 0.0 };
            geometry.material.shader = Shader::Lambert;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }
        { // light
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};

            parse_obj_file("data/cornell/luminaire.obj", true, &vertices, &indices, &cornell_aabb);
            GeometryInstance geometry = {};
            geometry.material.color  = { 50.0, 50.0, 50.0 };
            geometry.material.shader = Shader::Light;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }
        { // large box
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};

            parse_obj_file("data/cornell/largebox.obj", true, &vertices, &indices, &cornell_aabb);
            GeometryInstance geometry = {};
            geometry.material.color  = { 1.0, 1.0, 1.0 };
            geometry.material.shader = Shader::Translucent;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }
        { // small box
            Array<Vertex> vertices = {};
            Array<Index>  indices  = {};

            parse_obj_file("data/cornell/smallbox.obj", true, &vertices, &indices, &cornell_aabb);
            GeometryInstance geometry = {};
            geometry.material.color  = { 1.0, 1.0, 1.0 };
            geometry.material.shader = Shader::Translucent;
            geometry.vertices = vertices;
            geometry.indices  = indices;

            array_push(&all_vertices, vertices);
            array_push(&all_indices,  indices);
            array_push(&geometries, geometry);
        }

        cornell_blas = Raytracing::build_blas(cmd_list, geometries);

        for (auto& v : all_vertices) array_free(&v); array_free(&all_vertices);
        for (auto& i : all_indices)  array_free(&i); array_free(&all_indices);
        array_free(&geometries);

        // append instance
        BlasInstance instance = {};
        instance.blas        = &cornell_blas;

        float scale = 1.0 / aabb_widest(cornell_aabb);
        XMMATRIX transform = XMMatrixAffineTransformation(
            XMVectorReplicate(scale),
            g_XMZero, g_XMIdentityR3,
            (0.5*(cornell_aabb.max - cornell_aabb.min) - cornell_aabb.max) * scale
        );
        XMStoreFloat4x4(&instance.transform, transform);

        array_push(&instances, instance);
    }

    // Blas translucent_cube_blas; { // tranlucent cube
    //     Array<Vertex> vertices = {};
    //     Array<Index>  indices  = {};
    //     parse_obj_file("data/debug_cube.obj", false, &vertices, &indices);

    //     GeometryInstance geometry = {};
    //     geometry.material.color  = { 0.0, 0.0, 1.0 };
    //     geometry.material.shader = Shader::Translucent;
    //     geometry.vertices = vertices;
    //     geometry.indices  = indices;

    //     translucent_cube_blas = Raytracing::build_blas(cmd_list, array_of(&geometry));

    //     array_free(&vertices);
    //     array_free(&indices);
    // }

    // { // translucent cube #1
    //     const float cube_scale = 0.4;
    //     BlasInstance instance = {};
    //     instance.blas        = &translucent_cube_blas;

    //     XMMATRIX transform = XMMatrixScaling(cube_scale, cube_scale, cube_scale) * XMMatrixRotationZ(0.5*TAU + TAU/24) * XMMatrixRotationX(TAU/24) * XMMatrixTranslation(-0.15,0,0);
    //     XMStoreFloat4x4(&instance.transform, transform);

    //     array_push(&instances, instance);
    // };
    // { // translucent cube #2
    //     const float cube_scale = 0.1;
    //     BlasInstance instance = {};
    //     instance.blas        = &translucent_cube_blas;

    //     XMMATRIX transform = XMMatrixScaling(cube_scale, cube_scale, cube_scale) * XMMatrixRotationZ(-TAU/24) * XMMatrixRotationX(-TAU/24) * XMMatrixTranslation(0.15,0,0);
    //     XMStoreFloat4x4(&instance.transform, transform);

    //     array_push(&instances, instance);
    // };

    // finalize raytracing geometry and update descriptors
    Raytracing::build_tlas(cmd_list, instances);
    array_free(&instances);

    CHECK_RESULT(cmd_list->Close());
    g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
    Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);
    Device::release_temp_resources();

    // initialize globals
    Raytracing::g_globals.frame_rng = GetTickCount64(); // initialize random seed

    Raytracing::g_globals.samples_per_pixel  = 1;
    Raytracing::g_globals.bounces_per_sample = 4;
    Raytracing::g_globals.translucent_emission_bounces = 1;

    // Raytracing::g_globals.translucent_bssrdf_scale = 0.4;
    Raytracing::g_globals.translucent_bssrdf_scale = 0.0;
    Raytracing::g_globals.translucent_bssrdf_fudge = 1.0;
    Raytracing::g_globals.translucent_refractive_index = 1.75;
    Raytracing::g_globals.translucent_scattering = XMFLOAT3(15.0, 15.0, 15.0);
    Raytracing::g_globals.translucent_absorption = XMFLOAT3(0.1, 0.1, 0.1);

    // MAIN LOOP

    static UINT64 frame_id = 0;
    while (true) {
        // SETUP

        // event handler
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // synchronize with g_device
        // TODO: pipeline frames
        Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);
        Device::release_temp_resources();

        { // generate new rng seed for frame
            UINT rng = Raytracing::g_globals.frame_rng;
            // Thomas Wang hash
            // http://www.burtleburtle.net/bob/hash/integer.html
            rng = (rng ^ 61) ^ (rng >> 16);
            rng *= 9;
            rng = rng ^ (rng >> 4);
            rng *= 0x27d4eb2d;
            rng = rng ^ (rng >> 15);

            Raytracing::g_globals.frame_rng = rng;
        }

        if (g_do_update_resolution && !g_prevent_resizing) {
            g_do_update_resolution = false;
            g_do_reset_accumulator = true;
            update_resolution();
        }

        // poll mouse input
        XMFLOAT2 mouse_drag = {};
        float    mouse_scroll = 0;
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDragging(0)) {
                mouse_drag.x = (float) io.MouseDelta.x / (float) g_width;
                mouse_drag.y =-(float) io.MouseDelta.y / (float) g_width;
                g_do_reset_accumulator = true;
            }
            if (io.MouseWheel) {
                mouse_scroll = io.MouseWheel;
                g_do_reset_accumulator = true;
            };
        }

        // start imgui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // static bool show_demo_window = true; if (show_demo_window) ImGui::ShowDemoWindow(&show_demo_window);
        ImGui::Begin("debug menu");

        // UPDATE

        { // camera
            ImGui::Text("camera"); ImGui::SameLine();

            static float azimuth   = 0;
            static float elevation = 0;
            static float distance  = 2;
            static float target[3] = {};
            static float fov_x     = 70*DEGREES;
            static float fov_y     = fov_x / g_aspect;

            if (ImGui::Button("reset##camera") || frame_id == 0) {
                azimuth = 0;
                elevation = 9*DEGREES;
                distance = 2.5;
                target[0] = 0;
                target[1] = 0;
                target[2] = -0.06;
                fov_y = 30*DEGREES;
                fov_x = fov_y * g_aspect;
                g_do_reset_accumulator = true;
            }

            g_do_reset_accumulator |= ImGui::SliderAngle("azimuth##camera",   &azimuth);
            azimuth += mouse_drag.x * TAU/2;
            azimuth = fmod(azimuth + 3*TAU/2, TAU) - TAU/2;

            g_do_reset_accumulator |= ImGui::SliderAngle("elevation##camera", &elevation, -85, 85);
            elevation -= mouse_drag.y * TAU/2;
            elevation = clamp(elevation, -85*DEGREES, 85*DEGREES);

            g_do_reset_accumulator |= ImGui::DragFloat("distance##camera", &distance, distance*0.005, 0.001, FLT_MAX);
            distance -= mouse_scroll*distance*0.05;
            distance = clamp(distance, 0.005, INFINITY);

            g_do_reset_accumulator |= ImGui::SliderFloat3("focus##camera", target, -1, 1);

            if (ImGui::SliderAngle("fov x##camera", &fov_x, 5,          175))          { fov_y = fov_x / g_aspect; g_do_reset_accumulator = true; }
            if (ImGui::SliderAngle("fov y##camera", &fov_y, 5/g_aspect, 175/g_aspect)) { fov_x = fov_y * g_aspect; g_do_reset_accumulator = true; }

            XMVECTOR focus = XMLoadFloat3((XMFLOAT3*) target);
            XMVECTOR camera_pos = focus + XMVectorSet(
                -sinf(azimuth) * cosf(elevation) * distance,
                -cosf(azimuth) * cosf(elevation) * distance,
                                 sinf(elevation) * distance,
                1
            );
            Raytracing::g_globals.camera_aspect = g_aspect;
            Raytracing::g_globals.camera_focal_length = 1 / tanf(fov_y/2);
            XMMATRIX view = XMMatrixLookAtRH(camera_pos, focus, g_XMIdentityR2);
            XMStoreFloat4x4(&Raytracing::g_globals.camera_to_world, XMMatrixInverse(NULL, view));
        }

        { // translucent material
            ImGui::Separator();
            ImGui::Text("translucent material"); ImGui::SameLine();
            g_do_reset_accumulator |= ImGui::Checkbox("enabled##translucent", &Raytracing::g_enable_subsurface_scattering);

            g_do_reset_accumulator |= ImGui::SliderInt("emission bounces##render", (int*) &Raytracing::g_globals.translucent_emission_bounces, 0, Raytracing::g_globals.bounces_per_sample, "%d", ImGuiSliderFlags_AlwaysClamp);

            // static float scale = Raytracing::g_globals.translucent_bssrdf_scale;
            static float scale = 0.4;
            if (ImGui::RadioButton("tabulated", Raytracing::g_globals.translucent_bssrdf_scale != 0)) { Raytracing::g_globals.translucent_bssrdf_scale = scale; g_do_reset_accumulator = true; }; ImGui::SameLine();
            if (ImGui::RadioButton("dipole",    Raytracing::g_globals.translucent_bssrdf_scale == 0)) { Raytracing::g_globals.translucent_bssrdf_scale = 0;     g_do_reset_accumulator = true; };

            g_do_reset_translucent_accumulator |= ImGui::SliderFloat("refractive index##translucent", &Raytracing::g_globals.translucent_refractive_index, 1.0, 5.0, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);

            if (Raytracing::g_globals.translucent_bssrdf_scale != 0) {
                // tabulated
                g_do_reset_accumulator |= ImGui::SliderFloat("bssrdf radius##translucent", &Raytracing::g_globals.translucent_bssrdf_scale, 0.001, 100, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat);
                g_do_reset_accumulator |= ImGui::SliderFloat("bssrdf fudge##translucent", &Raytracing::g_globals.translucent_bssrdf_fudge, 0.0, 10000, "%.3f", ImGuiSliderFlags_Logarithmic);
                Raytracing::g_globals.translucent_bssrdf_scale = max(FLT_EPSILON, Raytracing::g_globals.translucent_bssrdf_scale);
                scale = Raytracing::g_globals.translucent_bssrdf_scale;
            } else {
                // dipole
                static float scattering_hue[3] = {1.0, 1.0, 1.0};
                static float scattering = 15.0;
                static float absorption_hue[3] = {1.0, 1.0, 1.0};
                static float absorption = 0.1;

                g_do_reset_accumulator |= ImGui::SliderFloat("scattering##translucent", &scattering, 0, 1000, "%.3f", ImGuiSliderFlags_Logarithmic);
                g_do_reset_accumulator |= ImGui::ColorEdit3("scattering rgb##translucent", scattering_hue, ImGuiColorEditFlags_Float);
                g_do_reset_accumulator |= ImGui::SliderFloat("absorption##translucent", &absorption, 0, 1000, "%.3f", ImGuiSliderFlags_Logarithmic);
                g_do_reset_accumulator |= ImGui::ColorEdit3("absorption rgb##translucent", absorption_hue, ImGuiColorEditFlags_Float);

                Raytracing::g_globals.translucent_scattering = { scattering_hue[0] * scattering, scattering_hue[1] * scattering, scattering_hue[2] * scattering };
                Raytracing::g_globals.translucent_absorption = { absorption_hue[0] * absorption, absorption_hue[1] * absorption, absorption_hue[2] * absorption };
            }
        }

        { // sample points
            ImGui::Separator();
            ImGui::Text("translucent samples"); ImGui::SameLine();
            g_do_reset_accumulator |= ImGui::Checkbox("enabled##sample_points", &Raytracing::g_enable_translucent_sample_collection);

            static float sample_point_radius = g_do_regenerate_translucent_samples;
            ImGui::SliderFloat("radius##sample_points", &sample_point_radius, 0.005, 0.5, "%.3f", ImGuiSliderFlags_Logarithmic);

            if (ImGui::Button("regenerate##sample_points")) { g_do_regenerate_translucent_samples = sample_point_radius; } ImGui::SameLine();
            g_do_reset_translucent_accumulator |= ImGui::Button("reset##sample_points");

            ImGui::Text("total sample points: %d", g_total_sample_points);
            ImGui::Text("accumulated samples: %d", Raytracing::g_globals.translucent_accumulator_count);
        }

        { // render settings
            ImGui::Separator();
            ImGui::Text("renderer");

            // int set_resolution[2] = { (int) g_width, (int) g_height };
            // if (ImGui::InputInt2("resolution", set_resolution, ImGuiInputTextFlags_EnterReturnsTrue)) {
            //     g_do_reset_accumulator = true;

            //     set_resolution[0] = max(set_resolution[0], 256);
            //     set_resolution[1] = max(set_resolution[1], 256);

            //     RECT rect;
            //     GetWindowRect(g_hwnd, &rect);
            //     rect.right  = rect.left + set_resolution[0];
            //     rect.bottom = rect.top  + set_resolution[1];
            //     AdjustWindowRectEx(&rect, WINDOW_STYLE, WINDOW_MENU, WINDOW_STYLE_EX);
            //     MoveWindow(g_hwnd, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, true);
            // }

            g_do_reset_accumulator |= ImGui::SliderInt("samples##render", (int*) &Raytracing::g_globals.samples_per_pixel,  1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            g_do_reset_accumulator |= ImGui::SliderInt("bounces##render", (int*) &Raytracing::g_globals.bounces_per_sample, 0, 16, "%d", ImGuiSliderFlags_AlwaysClamp);

            static bool accumulator = true;
            ImGui::Checkbox("sample accumulation##render", &accumulator);
            g_do_reset_accumulator |= !accumulator;
        }

        // PRE-RENDER

        // translucent samples
        if (g_do_regenerate_translucent_samples) {
            CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));

            g_total_sample_points = Raytracing::generate_translucent_samples(cmd_list, g_do_regenerate_translucent_samples, NULL);
            Raytracing::update_descriptors({ g_descriptor_heap, RAYTRACING_DESCRIPTOR_INDEX });

            CHECK_RESULT(cmd_list->Close());
            g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
            Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);
            Device::release_temp_resources();

            g_sample_points_radius = g_do_regenerate_translucent_samples;
            g_do_regenerate_translucent_samples = 0;
            g_do_reset_translucent_accumulator = true;
        }

        // frame accumulator
        if (g_do_reset_translucent_accumulator) Raytracing::g_globals.translucent_accumulator_count = 0;
        g_do_reset_accumulator |= g_do_reset_translucent_accumulator;
        g_do_reset_translucent_accumulator = false;

        if (g_do_reset_accumulator) Raytracing::g_globals.accumulator_count  = 0;
        g_do_reset_accumulator = false;

        // RENDER

        UINT backbuffer_index = g_swapchain->GetCurrentBackBufferIndex();

        CHECK_RESULT(g_cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));
        cmd_list->SetDescriptorHeaps(1, &g_descriptor_heap);

        // raytracing
        Raytracing::dispatch_rays(cmd_list);

        // copy raytracing output to backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Raytracing::g_render_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[backbuffer_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

        cmd_list->CopyResource(g_rtvs[backbuffer_index], Raytracing::g_render_target);

        { // image capture
            ImGui::Separator();
            ImGui::Text("image capture");
            static bool do_capture = false;
            static UINT capture_samples = 2048 * Raytracing::g_globals.samples_per_pixel;
            static ID3D12Resource* capture_readback_buffer = NULL;
            static UINT            capture_readback_buffer_pitch;

            if (do_capture && Raytracing::g_globals.accumulator_count == capture_samples) {
                // create readback buffer and add copy instructions to cmd_list
                do_capture = false;
                g_prevent_resizing += 1;

                capture_readback_buffer_pitch = round_up(g_width*4, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

                D3D12_RESOURCE_DESC resource_desc = {};
                resource_desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
                resource_desc.Format             = DXGI_FORMAT_UNKNOWN;
                resource_desc.Width              = capture_readback_buffer_pitch*g_height;
                resource_desc.Height             = 1;
                resource_desc.DepthOrArraySize   = 1;
                resource_desc.MipLevels          = 1;
                resource_desc.SampleDesc.Count   = 1;
                resource_desc.SampleDesc.Quality = 0;
                resource_desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                resource_desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

                CHECK_RESULT(g_device->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
                    &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                    NULL,
                    IID_PPV_ARGS(&capture_readback_buffer)
                ));
                SET_NAME(capture_readback_buffer);

                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = capture_readback_buffer;
                dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset             = 0;
                dst.PlacedFootprint.Footprint.Format   = PIXEL_FORMAT;
                dst.PlacedFootprint.Footprint.Width    = g_width;
                dst.PlacedFootprint.Footprint.Height   = g_height;
                dst.PlacedFootprint.Footprint.Depth    = 1;
                dst.PlacedFootprint.Footprint.RowPitch = capture_readback_buffer_pitch;

                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = Raytracing::g_render_target;
                src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;

                cmd_list->CopyTextureRegion(
                    &dst, 0, 0, 0,
                    &src, NULL
                );
                // wait until next frame for copy to complete
            } else if (capture_readback_buffer) {
                // write file from readback buffer and release it
                static_assert(PIXEL_FORMAT == DXGI_FORMAT_R8G8B8A8_UNORM, "");

                void* data_ptr;
                CHECK_RESULT(capture_readback_buffer->Map(0, NULL, &data_ptr));

                // write image file
                char timestamp[32] = {};
                const time_t now = time(NULL);
                strftime(timestamp, 32, "%Y_%m_%d_%H_%M_%S", gmtime(&now));
                char filename[128] = {};
                sprintf(filename, "captures/%s_%s-lambda%.4f-mus(%.3e,%.3e,%.3e)-mua(%.3e,%.3e,%.3e)-r%.3e-#%d-%dx%d.png", SCENE_NAME, timestamp,
                    Raytracing::g_globals.translucent_refractive_index,
                    Raytracing::g_globals.translucent_scattering.x, Raytracing::g_globals.translucent_scattering.y, Raytracing::g_globals.translucent_scattering.z,
                    Raytracing::g_globals.translucent_absorption.x, Raytracing::g_globals.translucent_absorption.y, Raytracing::g_globals.translucent_absorption.z,
                    g_sample_points_radius, capture_samples, g_width, g_height
                );
                stbi_write_png(filename, g_width, g_height, 4, data_ptr, capture_readback_buffer_pitch);

                capture_readback_buffer->Unmap(0, &CD3DX12_RANGE(0, 0));
                capture_readback_buffer->Release();
                capture_readback_buffer = NULL;

                g_prevent_resizing -= 1;
            }

            bool capture_updated = false;
            capture_updated |= ImGui::SliderInt("samples##capture", (int*) &capture_samples, Raytracing::g_globals.samples_per_pixel, 32768, "%d", ImGuiSliderFlags_AlwaysClamp);
            capture_samples = round_up(ensure_unsigned(capture_samples), Raytracing::g_globals.samples_per_pixel);

            capture_updated |= ImGui::Checkbox("capture", &do_capture);

            ImGui::Text("accumulated samples: %d", Raytracing::g_globals.accumulator_count);
        }

        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(Raytracing::g_render_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        // FINALIZE

        // render imgui
        ImGui::End();
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[backbuffer_index], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET));
        auto rtv_descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), backbuffer_index, g_rtv_descriptor_size);
        cmd_list->OMSetRenderTargets(1, &rtv_descriptor, FALSE, NULL);

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

        // present backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[backbuffer_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        CHECK_RESULT(cmd_list->Close());

        // execute
        g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);

        CHECK_RESULT(g_swapchain->Present(VSYNC, 0));

        frame_id += 1;
    }
}
