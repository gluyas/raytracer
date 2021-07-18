#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "prelude.h"

#include "device.h"
using Device::g_device;
using Device::g_descriptor_size;
using Device::g_rtv_descriptor_size;

#include "parse_obj.h"
#include "bluenoise.h"

#include "out/raytracing_hlsl.h"

#define SCENE_NAME "sample_points_test"

#define WINDOW_STYLE_EX WS_EX_OVERLAPPEDWINDOW
#define WINDOW_STYLE (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
#define WINDOW_MENU 0

#define MIN_RESOLUTION 256

#define VSYNC 0
#define PIXEL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

#define SWAPCHAIN_BUFFER_COUNT 2

// TODO: move to same header as HLSL root signature definition
#define IMGUI_DESCRIPTOR_INDEX 0
#define DESCRIPTORS_COUNT 7

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

ID3D12Resource* g_blas_buffer = NULL;
ID3D12Resource* g_tlas_buffer = NULL;

ID3D12StateObject*           g_raytracing_pso        = NULL;
ID3D12StateObjectProperties* g_raytracing_properties = NULL;

ID3D12RootSignature* g_raytracing_global_root_signature = NULL;

Descriptor g_raytracing_render_target      = {};
Descriptor g_raytracing_sample_accumulator = {};
Descriptor*     g_raytracing_output_descriptors[] = { &g_raytracing_render_target, &g_raytracing_sample_accumulator };
DescriptorTable g_raytracing_output_dt            = { 1, VLA_VIEW(g_raytracing_output_descriptors) };

Descriptor g_raytracing_vertices = {};
Descriptor g_raytracing_indices  = {};
Descriptor*     g_raytracing_geometry_descriptors[] = { &g_raytracing_vertices, &g_raytracing_indices };
DescriptorTable g_raytracing_geometry_dt            = { DescriptorTable::offset_after(&g_raytracing_output_dt), VLA_VIEW(g_raytracing_geometry_descriptors) };

Descriptor g_translucent_samples          = {};
Descriptor g_translucent_tabulated_bssrdf = {};
Descriptor*     g_raytracing_translucent_descriptors[] = { &g_translucent_samples, &g_translucent_tabulated_bssrdf };
DescriptorTable g_raytracing_translucent_dt            = { DescriptorTable::offset_after(&g_raytracing_geometry_dt), VLA_VIEW(g_raytracing_translucent_descriptors) };

ID3D12Resource*   g_raytracing_globals_buffer = {};
RaytracingGlobals g_raytracing_globals        = {};

RootArgument g_raytracing_root_arguments[] = {
    RootArgument::cbv(&g_raytracing_globals_buffer, L"g_raytracing_globals"),
    RootArgument::descriptor_table(&g_raytracing_output_dt, L"g_raytracing_output_dt"),
    RootArgument::srv(&g_tlas_buffer, L"g_scene"),
    RootArgument::descriptor_table(&g_raytracing_geometry_dt, L"g_raytracing_geometry_dt"),
    RootArgument::descriptor_table(&g_raytracing_translucent_dt, L"g_raytracing_translucent_dt")
};

__declspec(align(32)) struct ShaderIdentifier {
    char bytes[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
};

// shader record for chit shaders
__declspec(align(32)) struct ShaderRecord {
    ShaderIdentifier ident;
    RaytracingLocals locals;
};

ID3D12Resource* g_camera_rgen_shader_record      = NULL;
ID3D12Resource* g_translucent_rgen_shader_record = NULL;
ID3D12Resource* g_hit_group_shader_table = NULL;
ID3D12Resource* g_miss_shader_table      = NULL;

bool g_do_collect_translucent_samples = true;
bool g_do_update_resolution = true;
bool g_do_reset_accumulator = true;
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
        swapchain1->Release();
    } else {
        // release rtv references
        for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
            g_rtvs[i]->Release();
        }
        g_swapchain->ResizeBuffers(SWAPCHAIN_BUFFER_COUNT, g_width, g_height, PIXEL_FORMAT, 0);
    }

    { // g_rtvs
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
            CHECK_RESULT(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_rtvs[i])));
            g_device->CreateRenderTargetView(g_rtvs[i], NULL, rtv_handle);
            rtv_handle.ptr += g_rtv_descriptor_size;
        }
    }

    { // g_raytracing_output_dt, g_raytracing_render_target, g_raytracing_sample_accumulator
        D3D12_RESOURCE_DESC rt_resource_desc = {};
        rt_resource_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_resource_desc.Width              = g_width;
        rt_resource_desc.Height             = g_height;
        rt_resource_desc.DepthOrArraySize   = 1;
        rt_resource_desc.MipLevels          = 1;
        rt_resource_desc.SampleDesc.Count   = 1;
        rt_resource_desc.SampleDesc.Quality = 0;
        rt_resource_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rt_resource_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        { // g_raytracing_render_target
            if (g_raytracing_render_target.resource) g_raytracing_render_target.resource->Release();

            D3D12_RESOURCE_DESC resource_desc = rt_resource_desc;
            resource_desc.Format              = PIXEL_FORMAT;

            ID3D12Resource* resource;
            CHECK_RESULT(g_device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
                &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                NULL,
                IID_PPV_ARGS(&resource)
            ));

           g_raytracing_render_target = Descriptor::uav_with_default_desc(resource, L"raytracing_render_target");
        }

        { // g_raytracing_sample_accumulator
            if (g_raytracing_sample_accumulator.resource) g_raytracing_sample_accumulator.resource->Release();

            D3D12_RESOURCE_DESC resource_desc = rt_resource_desc;
            resource_desc.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;

            ID3D12Resource* resource;
            CHECK_RESULT(g_device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
                &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                NULL,
                IID_PPV_ARGS(&resource)
            ));

           g_raytracing_sample_accumulator = Descriptor::uav_with_default_desc(resource, L"raytracing_sample_accumulator");
        }

        set_descriptor_table_on_heap(g_descriptor_heap, &g_raytracing_output_dt);
    }
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

ArrayView<void> get_shader_identifier(LPCWSTR name) {
    return array_from(g_raytracing_properties->GetShaderIdentifier(name), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
}

void prepare_raytracing_pipeline(ID3D12GraphicsCommandList4* cmd_list) {
    cmd_list->SetDescriptorHeaps(1, &g_descriptor_heap);
    cmd_list->SetComputeRootSignature(g_raytracing_global_root_signature);
    set_root_arguments(cmd_list, g_descriptor_heap, VLA_VIEW(g_raytracing_root_arguments));
    cmd_list->SetPipelineState1(g_raytracing_pso);
}

void generate_translucent_tabulated_bssrdf(ID3D12GraphicsCommandList4* cmd_list, float max_radius, UINT count) {
    g_raytracing_globals.translucent_tabulated_bssrdf_stepsize = max_radius / count;

    // TODO: use MC-generated bssrdf values
    Array<XMFLOAT4> bssrdf = array_init<XMFLOAT4>(count);
    for (UINT i = 0; i < count; i++) {
        float sigma = max_radius / 6;
        float r = max_radius * (float) i / (float) (count - 1);
        XMFLOAT4 f;
        f.w = r;
        f.x = expf(-r / sigma) / sigma;
        sigma *= 0.75;
        f.y = expf(-r / sigma) / sigma;
        sigma *= 0.75;
        f.z = expf(-r / sigma) / sigma;
        array_push(&bssrdf, f);
    }

    cmd_list->Reset(g_cmd_allocator, NULL);

    ID3D12Resource* upload_buffer = NULL;
    { // g_translucent_tabulated_bssrdf
        if (g_translucent_tabulated_bssrdf.resource) g_translucent_tabulated_bssrdf.resource->Release();

        ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, bssrdf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &upload_buffer);

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Buffer.NumElements      = count;

        g_translucent_tabulated_bssrdf = Descriptor::srv(buffer, desc, L"translucent_tabulated_bssrdf");
    }
    set_descriptor_table_on_heap(g_descriptor_heap, &g_raytracing_translucent_dt);

    cmd_list->Close();
    g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
    Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);

    upload_buffer->Release();
    array_free(&bssrdf);
}

void collect_translucent_samples(ID3D12GraphicsCommandList4* cmd_list, float radius, UINT radiance_samples_per_point) {
    Array<Vertex> vertices = {};
    Array<Index>  indices  = {};
    parse_obj_file("data/debug_cube.obj", &vertices, &indices);

    UINT total_samples;
    {
        if (g_translucent_samples.resource) g_translucent_samples.resource->Release();

        ID3D12Resource* sample_points_buffer;
        total_samples = Bluenoise::generate_sample_points(&sample_points_buffer, vertices, indices, radius);
        if (total_samples == 0) abort();

        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format        = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement        = 0;
        desc.Buffer.NumElements         = total_samples;
        desc.Buffer.StructureByteStride = sizeof(SamplePoint);

        g_translucent_samples = Descriptor::uav(sample_points_buffer, desc, L"translucent_samples");
    }
    set_descriptor_table_on_heap(g_descriptor_heap, &g_raytracing_translucent_dt);

    // update globals
    RaytracingGlobals temp_globals = g_raytracing_globals;
    temp_globals.samples_per_pixel = radiance_samples_per_point;
    temp_globals.translucent_samples_count = 0;
    copy_to_upload_buffer(g_raytracing_globals_buffer, array_of(&temp_globals));

    // sample collection
    D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
    dispatch_rays.HitGroupTable.StartAddress  = g_hit_group_shader_table->GetGPUVirtualAddress();
    dispatch_rays.HitGroupTable.SizeInBytes   = g_hit_group_shader_table->GetDesc().Width;
    dispatch_rays.HitGroupTable.StrideInBytes = sizeof(ShaderRecord);

    dispatch_rays.MissShaderTable.StartAddress  = g_miss_shader_table->GetGPUVirtualAddress();
    dispatch_rays.MissShaderTable.SizeInBytes   = g_miss_shader_table->GetDesc().Width;
    dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // TODO: calculate this

    dispatch_rays.RayGenerationShaderRecord.StartAddress = g_translucent_rgen_shader_record->GetGPUVirtualAddress();
    dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = g_translucent_rgen_shader_record->GetDesc().Width;

    dispatch_rays.Width  = total_samples;
    dispatch_rays.Height = 1;
    dispatch_rays.Depth  = 1;

    CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));
    prepare_raytracing_pipeline(cmd_list);
    cmd_list->DispatchRays(&dispatch_rays);

    CHECK_RESULT(cmd_list->Close());
    g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
    Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);

    // restore global state
    g_raytracing_globals.translucent_samples_count = total_samples;
    copy_to_upload_buffer(g_raytracing_globals_buffer, array_of(&g_raytracing_globals));
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
        rect.right  = 1600;
        rect.bottom = 900;
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
    Bluenoise::init();

    // PIPELINE

    { // g_cmd_queue
        D3D12_COMMAND_QUEUE_DESC g_cmd_queue_desc = {};
        g_cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        g_cmd_queue_desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;

        CHECK_RESULT(g_device->CreateCommandQueue(&g_cmd_queue_desc, IID_PPV_ARGS(&g_cmd_queue)));
    }

    { // g_rtv_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_rtv_descriptor_heap_desc = {};
        g_rtv_descriptor_heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        g_rtv_descriptor_heap_desc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;

        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_rtv_descriptor_heap_desc, IID_PPV_ARGS(&g_rtv_descriptor_heap)));
    }

    // g_fence, g_Fence::event
    g_fence = Fence::make();

    { // g_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_descriptor_heap_desc = {};
        g_descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        g_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_descriptor_heap_desc.NumDescriptors = DESCRIPTORS_COUNT;
        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_descriptor_heap_desc, IID_PPV_ARGS(&g_descriptor_heap)));
    }

    // SETUP

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

    // RAYTRACING PIPELINE

    CHECK_RESULT(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_allocator)));

    ID3D12GraphicsCommandList4* cmd_list = NULL;
    CHECK_RESULT(g_device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmd_list)));

    { // g_raytracing_pso, g_raytracing_properties
        auto pso_desc = CD3DX12_STATE_OBJECT_DESC(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

        auto dxil_subobject = pso_desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>(); {
            auto bytecode = CD3DX12_SHADER_BYTECODE((void *) g_raytracing_hlsl_bytecode, _countof(g_raytracing_hlsl_bytecode));
            dxil_subobject->SetDXILLibrary(&bytecode);
        }

        // auto lambert_hit_group_subobject = pso_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>(); {
        //     lambert_hit_group_subobject->SetClosestHitShaderImport(L"lambert_chit");
        //     lambert_hit_group_subobject->SetHitGroupExport(L"lambert_hit_group");
        //     lambert_hit_group_subobject->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
        // }

        CHECK_RESULT(g_device->CreateStateObject(pso_desc, IID_PPV_ARGS(&g_raytracing_pso)));
        CHECK_RESULT(g_raytracing_pso->QueryInterface(IID_PPV_ARGS(&g_raytracing_properties)));
    }

    // g_raytracing_global_root_signature
    CHECK_RESULT(g_device->CreateRootSignature(0, g_raytracing_hlsl_bytecode, _countof(g_raytracing_hlsl_bytecode), IID_PPV_ARGS(&g_raytracing_global_root_signature)));

    // ASSET LOADING

    enum Material {
        Lambert,
        Light,
        Translucent,

        Count,
    };

    void* material_hit_group_identifiers[Material::Count] = {}; {
        material_hit_group_identifiers[Lambert]     = g_raytracing_properties->GetShaderIdentifier(L"lambert_hit_group");
        material_hit_group_identifiers[Light]       = g_raytracing_properties->GetShaderIdentifier(L"light_hit_group");
        material_hit_group_identifiers[Translucent] = g_raytracing_properties->GetShaderIdentifier(L"translucent_hit_group");
    }

    struct GeometryInstance {
        const char* filename;
        Material material;
        XMFLOAT3 color;
    };

    // TODO: use actual reflectance values
    // TODO: merge geometries with identical local arguments
    GeometryInstance scene_objects[] = {
        { "data/debug_cube.obj", Translucent, { 1, 1, 1 } },

        { "data/cornell/floor.obj",     Lambert, { 1, 1, 1 } },
        { "data/cornell/back.obj",      Lambert, { 1, 1, 1 } },
        { "data/cornell/ceiling.obj",   Lambert, { 1, 1, 1 } },
        { "data/cornell/greenwall.obj", Lambert, { 0, 1, 0 } },
        { "data/cornell/redwall.obj",   Lambert, { 1, 0, 0 } },
        // { "data/cornell/largebox.obj",  Lambert, { 1, 1, 1 } },
        // { "data/cornell/smallbox.obj",  Lambert, { 1, 1, 1 } },

        { "data/cornell/luminaire.obj", Light, { 50, 50, 50 } },
    };

    { // initialize raytracing pipeline
        // TODO: factor this procedure somehow
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_descs[_countof(scene_objects)] = {};

        // parse meshes into a single pair of index and vertex buffers
        Array<Vertex> vb_data = {};
        Array<Index>  ib_data = {};
        Aabb aabb = AABB_NULL;
        for (int i = 0; i < _countof(scene_objects); i++) {
            // record offset into the ib array - combine with virtual address after geometry upload
            Index ib_offset = ib_data.len;
            geometry_descs[i].Triangles.IndexBuffer = ib_offset * sizeof(Index);

            // append mesh data to shared ib and vb
            parse_obj_file(scene_objects[i].filename, &vb_data, &ib_data, &aabb);

            // record number of indices
            geometry_descs[i].Triangles.IndexCount  = ib_data.len - ib_offset;
        }
        // pad index buffer
        while (ib_data.len % 4 != 0) array_push(&ib_data, (Index) 0);

        // upload data
        cmd_list->Reset(g_cmd_allocator, NULL);

        // rgen shader records
        g_camera_rgen_shader_record      = create_buffer_and_write_contents(cmd_list, get_shader_identifier(L"camera_rgen"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        SET_NAME(g_camera_rgen_shader_record);
        g_translucent_rgen_shader_record = create_buffer_and_write_contents(cmd_list, get_shader_identifier(L"translucent_rgen"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        SET_NAME(g_translucent_rgen_shader_record);

        // build shader tables
        // these need access to the index buffer offsets
        g_hit_group_shader_table = NULL; {
            ShaderRecord shader_table[_countof(scene_objects)] = {};
            for (int i = 0; i < _countof(scene_objects); i++) {
                void* hit_group_ident = material_hit_group_identifiers[scene_objects[i].material];
                memcpy((void*) &shader_table[i].ident, hit_group_ident, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                shader_table[i].locals.color = scene_objects[i].color;
                shader_table[i].locals.primitive_index_offset = geometry_descs[i].Triangles.IndexBuffer / sizeof(Index) / 3;
            }

            g_hit_group_shader_table = create_buffer_and_write_contents(cmd_list, VLA_VIEW(shader_table), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
            SET_NAME(g_hit_group_shader_table);
        }

        g_miss_shader_table = create_buffer_and_write_contents(cmd_list, get_shader_identifier(L"miss"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        SET_NAME(g_miss_shader_table);

        // upload geometry and create SRVs
        { // g_raytracing_vertices
            ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, vb_data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);

            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format                  = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            desc.Buffer.NumElements         = vb_data.len;
            desc.Buffer.StructureByteStride = sizeof(Vertex);

            g_raytracing_vertices = Descriptor::srv(buffer, desc, L"vertices");
        }
        { // g_raytracing_indices
            ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, ib_data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            desc.Buffer.NumElements = ib_data.len*sizeof(Index) / 4;
            desc.Buffer.Flags       = D3D12_BUFFER_SRV_FLAG_RAW;

            g_raytracing_indices = Descriptor::srv(buffer, desc, L"indices");
        }
        set_descriptor_table_on_heap(g_descriptor_heap, &g_raytracing_geometry_dt);

        cmd_list->Close();
        g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
        Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);
        Device::release_temp_resources();

        // finalize geometry_descs
        for (int i = 0; i < _countof(geometry_descs); i++) {
            geometry_descs[i].Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry_descs[i].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // disables any-hit shader

            geometry_descs[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry_descs[i].Triangles.VertexCount  = vb_data.len;
            geometry_descs[i].Triangles.VertexBuffer.StartAddress  = g_raytracing_vertices.resource->GetGPUVirtualAddress();
            geometry_descs[i].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

            geometry_descs[i].Triangles.IndexFormat  = DXGI_FORMAT_R16_UINT;
            // IndexCount was written in previous loop; add address to the offset stored in IndexBuffer
            geometry_descs[i].Triangles.IndexBuffer += g_raytracing_indices.resource->GetGPUVirtualAddress();
        }

        // build raytracing geometry
        // TODO: extract into a callable procedure
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build = {};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build = {};

        // blas inputs
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* blas_inputs = &blas_build.Inputs;
        blas_inputs->Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs->Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blas_inputs->DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs->NumDescs       = _countof(geometry_descs);
        blas_inputs->pGeometryDescs = geometry_descs;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_prebuild = {};
        g_device->GetRaytracingAccelerationStructurePrebuildInfo(blas_inputs, &blas_prebuild);

        // tlas inputs
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* tlas_inputs = &tlas_build.Inputs;
        tlas_inputs->Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs->Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlas_inputs->DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlas_inputs->NumDescs       = 1;
        tlas_inputs->InstanceDescs  = NULL; // wait until blas is built on g_device

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_prebuild = {};
        g_device->GetRaytracingAccelerationStructurePrebuildInfo(tlas_inputs, &tlas_prebuild);

        // allocate buffers and build
        auto buffer_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto buffer_desc       = CD3DX12_RESOURCE_DESC::Buffer(0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        // scratch buffer
        buffer_desc.Width = fmax(blas_prebuild.ScratchDataSizeInBytes, tlas_prebuild.ScratchDataSizeInBytes);
        ID3D12Resource* scratch_buffer = NULL;
        CHECK_RESULT(g_device->CreateCommittedResource(
            &buffer_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&scratch_buffer)
        ));
        SET_NAME(scratch_buffer);

        // blas build
        buffer_desc.Width = blas_prebuild.ResultDataMaxSizeInBytes;
        CHECK_RESULT(g_device->CreateCommittedResource(
            &buffer_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            NULL,
            IID_PPV_ARGS(&g_blas_buffer)
        ));
        SET_NAME(g_blas_buffer);
        blas_build.DestAccelerationStructureData    = g_blas_buffer->GetGPUVirtualAddress();
        blas_build.SourceAccelerationStructureData  = NULL;
        blas_build.ScratchAccelerationStructureData = scratch_buffer->GetGPUVirtualAddress();

        // tlas instance descs
        ID3D12Resource* instance_descs_buffer = NULL; {
            XMFLOAT3 m; XMStoreFloat3(&m, aabb.min);
            XMFLOAT3 w; XMStoreFloat3(&w, aabb.max-aabb.min);

            float scale = 0;
            scale = fmax(w.x, scale);
            scale = fmax(w.y, scale);
            scale = fmax(w.z, scale);
            scale = 1/scale;

            D3D12_RAYTRACING_INSTANCE_DESC instance_desc = {};
            instance_desc.Transform[0][0] =-scale;
            instance_desc.Transform[1][2] = scale;
            instance_desc.Transform[2][1] = scale;
            instance_desc.Transform[0][3] = scale*(m.x + w.x*0.5);
            instance_desc.Transform[2][3] =-scale*(m.y + w.y*0.5);
            instance_desc.Transform[1][3] =-scale*(m.z + w.z*0.5);
            instance_desc.InstanceMask = 1;
            instance_desc.AccelerationStructure = g_blas_buffer->GetGPUVirtualAddress();

            instance_descs_buffer = create_upload_buffer(array_of(&instance_desc));
            SET_NAME(instance_descs_buffer);

            // HACK: apply inverse transform to debug_cube.obj to give it absolute coordinates
            XMFLOAT3X4 inverse_transform; {
                XMMATRIX matrix = XMLoadFloat3x4((XMFLOAT3X4*) &instance_desc.Transform);
                matrix = XMMatrixInverse(NULL, matrix);
                XMStoreFloat3x4(&inverse_transform, matrix);
            }
            inverse_transform._22 *= -1;
            inverse_transform._33 *= -1;
            swap(&inverse_transform._33, &inverse_transform._23);
            swap(&inverse_transform._22, &inverse_transform._32);

            geometry_descs[0].Triangles.Transform3x4 = create_upload_buffer(array_of(&inverse_transform))->GetGPUVirtualAddress();
        }

        // tlas build
        tlas_inputs->InstanceDescs = instance_descs_buffer->GetGPUVirtualAddress();

        buffer_desc.Width = tlas_prebuild.ResultDataMaxSizeInBytes;
        CHECK_RESULT(g_device->CreateCommittedResource(
            &buffer_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            NULL,
            IID_PPV_ARGS(&g_tlas_buffer)
        ));
        SET_NAME(g_tlas_buffer);
        tlas_build.DestAccelerationStructureData    = g_tlas_buffer->GetGPUVirtualAddress();
        tlas_build.SourceAccelerationStructureData  = NULL;
        tlas_build.ScratchAccelerationStructureData = scratch_buffer->GetGPUVirtualAddress();

        // execute build
        CHECK_RESULT(g_cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));
        cmd_list->BuildRaytracingAccelerationStructure(&blas_build, 0, NULL);
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(scratch_buffer));
        cmd_list->BuildRaytracingAccelerationStructure(&tlas_build, 0, NULL);
        CHECK_RESULT(cmd_list->Close());

        g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
        Fence::increment_and_signal_and_wait(g_cmd_queue, &g_fence);

        scratch_buffer->Release();
    }

    g_raytracing_globals_buffer = create_upload_buffer(sizeof(g_raytracing_globals));
    g_raytracing_globals.samples_per_pixel  = 1;
    g_raytracing_globals.bounces_per_sample = 4;
    g_raytracing_globals.frame_rng = GetTickCount64(); // initialize random seed

    g_raytracing_globals.translucent_absorption = 0.01;
    g_raytracing_globals.translucent_scattering = 50;
    g_raytracing_globals.translucent_refraction = 1.35;

    g_raytracing_globals.translucent_tabulated_bssrdf_count = 32;
    generate_translucent_tabulated_bssrdf(cmd_list, 0.5, g_raytracing_globals.translucent_tabulated_bssrdf_count);

    // MAIN LOOP

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
            UINT rng = g_raytracing_globals.frame_rng;
            // Thomas Wang hash
            // http://www.burtleburtle.net/bob/hash/integer.html
            rng = (rng ^ 61) ^ (rng >> 16);
            rng *= 9;
            rng = rng ^ (rng >> 4);
            rng *= 0x27d4eb2d;
            rng = rng ^ (rng >> 15);

            g_raytracing_globals.frame_rng = rng;
        }

        if (g_do_update_resolution && !g_prevent_resizing) {
            g_do_update_resolution = false;
            g_do_reset_accumulator = true;
            update_resolution();
        }

        static float translucent_sample_point_radius = 0.01;
        static UINT translucent_samples_per_point    = 16384;
        if (g_do_collect_translucent_samples) {
            g_do_collect_translucent_samples = false;
            g_do_reset_accumulator           = true;

            CHECK_RESULT(g_cmd_allocator->Reset());
            collect_translucent_samples(cmd_list, translucent_sample_point_radius, translucent_samples_per_point);
            set_descriptor_table_on_heap(g_descriptor_heap, &g_raytracing_translucent_dt);
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
            ImGui::Text("camera");

            static float azimuth   = 0;
            static float elevation = 0;
            static float distance  = 2;
            static float fov_x     = 70*DEGREES;
            static float fov_y     = fov_x / g_aspect;

            g_do_reset_accumulator |= ImGui::SliderAngle("azimuth",   &azimuth);
            azimuth += mouse_drag.x * TAU/2;
            azimuth = fmod(azimuth + 3*TAU/2, TAU) - TAU/2;

            g_do_reset_accumulator |= ImGui::SliderAngle("elevation", &elevation, -85, 85);
            elevation -= mouse_drag.y * TAU/2;
            elevation = clamp(elevation, -85*DEGREES, 85*DEGREES);

            g_do_reset_accumulator |= ImGui::DragFloat("distance", &distance, distance*0.005, 0.001, FLT_MAX);
            distance -= mouse_scroll*distance*0.05;
            distance = clamp(distance, 0.005, INFINITY);

            if (ImGui::SliderAngle("fov x", &fov_x, 5,          175))          { fov_y = fov_x / g_aspect; g_do_reset_accumulator = true; }
            if (ImGui::SliderAngle("fov y", &fov_y, 5/g_aspect, 175/g_aspect)) { fov_x = fov_y * g_aspect; g_do_reset_accumulator = true; }

            XMVECTOR camera_pos = XMVectorSet(
                -sinf(azimuth) * cosf(elevation) * distance,
                -cosf(azimuth) * cosf(elevation) * distance,
                                 sinf(elevation) * distance,
                1
            );
            g_raytracing_globals.camera_aspect = g_aspect;
            g_raytracing_globals.camera_focal_length = 1 / tanf(fov_y/2);
            XMMATRIX view = XMMatrixLookAtRH(camera_pos, g_XMZero, g_XMIdentityR2);
            XMStoreFloat4x4(&g_raytracing_globals.camera_to_world, XMMatrixInverse(NULL, view));
        }

        { // render settings
            ImGui::Text("renderer");

            int set_resolution[2] = { (int) g_width, (int) g_height };
            if (ImGui::InputInt2("resolution", set_resolution, ImGuiInputTextFlags_EnterReturnsTrue)) {
                g_do_reset_accumulator = true;

                set_resolution[0] = max(set_resolution[0], 256);
                set_resolution[1] = max(set_resolution[1], 256);

                RECT rect;
                GetWindowRect(g_hwnd, &rect);
                rect.right  = rect.left + set_resolution[0];
                rect.bottom = rect.top  + set_resolution[1];
                AdjustWindowRectEx(&rect, WINDOW_STYLE, WINDOW_MENU, WINDOW_STYLE_EX);
                MoveWindow(g_hwnd, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top, true);
            }

            g_do_reset_accumulator |= ImGui::SliderInt("samples##render", (int*) &g_raytracing_globals.samples_per_pixel,  1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            g_do_reset_accumulator |= ImGui::SliderInt("bounces##render", (int*) &g_raytracing_globals.bounces_per_sample, 0, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
        }

        { // translucent material
            ImGui::Text("translucent material");
            g_do_reset_accumulator |= ImGui::SliderFloat("absorption", &g_raytracing_globals.translucent_absorption, 0.0, 50.0);
            g_do_reset_accumulator |= ImGui::SliderFloat("scattering", &g_raytracing_globals.translucent_scattering, 0.0, 500.0);
            g_do_reset_accumulator |= ImGui::SliderFloat("refractive index", &g_raytracing_globals.translucent_refraction, 1.0, 2.0);

            static bool use_tabulated_bssrdf = true;
            if (ImGui::Checkbox("use tabulated", &use_tabulated_bssrdf)) {
                static UINT toggle = 0;
                swap(&toggle, &g_raytracing_globals.translucent_tabulated_bssrdf_count);
                g_do_reset_accumulator = true;
            }
        }

        { // translucent samples
            ImGui::Text("translucent samples");
            ImGui::SliderFloat("radius##translucent", &translucent_sample_point_radius, 0.001, 0.3, "%.3f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderInt("samples##translucent", (int*) &translucent_samples_per_point,    1, 32768, "%d", ImGuiSliderFlags_AlwaysClamp);
            g_do_collect_translucent_samples |= ImGui::Button("resample");
        }

        // frame accumulator
        if (g_do_reset_accumulator) g_raytracing_globals.accumulator_count  = 0;
        g_do_reset_accumulator = false;

        // upload frame constants
        copy_to_upload_buffer(g_raytracing_globals_buffer, array_of(&g_raytracing_globals));
        g_raytracing_globals.accumulator_count += g_raytracing_globals.samples_per_pixel;

        // RENDER

        UINT frame_index = g_swapchain->GetCurrentBackBufferIndex();

        CHECK_RESULT(g_cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, NULL));

        // raytracing
        D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
        dispatch_rays.HitGroupTable.StartAddress  = g_hit_group_shader_table->GetGPUVirtualAddress();
        dispatch_rays.HitGroupTable.SizeInBytes   = g_hit_group_shader_table->GetDesc().Width;
        dispatch_rays.HitGroupTable.StrideInBytes = sizeof(ShaderRecord);

        dispatch_rays.MissShaderTable.StartAddress  = g_miss_shader_table->GetGPUVirtualAddress();
        dispatch_rays.MissShaderTable.SizeInBytes   = g_miss_shader_table->GetDesc().Width;
        dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // TODO: calculate this

        dispatch_rays.RayGenerationShaderRecord.StartAddress = g_camera_rgen_shader_record->GetGPUVirtualAddress();
        dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = g_camera_rgen_shader_record->GetDesc().Width;

        dispatch_rays.Width  = g_width;
        dispatch_rays.Height = g_height;
        dispatch_rays.Depth  = 1;

        prepare_raytracing_pipeline(cmd_list);
        cmd_list->DispatchRays(&dispatch_rays);
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(g_raytracing_render_target.resource));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(g_raytracing_sample_accumulator.resource));

        // copy raytracing output to backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_raytracing_render_target.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[frame_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

        cmd_list->CopyResource(g_rtvs[frame_index], g_raytracing_render_target.resource);

        { // image capture
            ImGui::Text("image capture");
            static bool do_capture = false;
            static UINT capture_samples = 1024 * g_raytracing_globals.samples_per_pixel;
            static ID3D12Resource* capture_readback_buffer = NULL;
            static UINT            capture_readback_buffer_pitch;

            if (do_capture && g_raytracing_globals.accumulator_count == capture_samples) {
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
                src.pResource = g_raytracing_render_target.resource;
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
                sprintf(filename, "captures/%s_%s.%dx%d.s%d.b%d.png", SCENE_NAME, timestamp, g_width, g_height, capture_samples, g_raytracing_globals.bounces_per_sample);
                stbi_write_png(filename, g_width, g_height, 4, data_ptr, capture_readback_buffer_pitch);

                capture_readback_buffer->Unmap(0, &CD3DX12_RANGE(0, 0));
                capture_readback_buffer->Release();
                capture_readback_buffer = NULL;

                g_prevent_resizing -= 1;
            }

            bool capture_updated = false;
            capture_updated |= ImGui::SliderInt("samples##capture", (int*) &capture_samples, g_raytracing_globals.samples_per_pixel, 32768, "%d", ImGuiSliderFlags_AlwaysClamp);
            capture_samples = round_up(ensure_unsigned(capture_samples), g_raytracing_globals.samples_per_pixel);

            capture_updated |= ImGui::Checkbox("capture", &do_capture);

            if (capture_updated && do_capture && g_raytracing_globals.accumulator_count > capture_samples) {
                // reset accumulator if capture is set after specified samples has already been reached
                g_do_reset_accumulator = true;
            }

            ImGui::Text("accumulated samples: %d", g_raytracing_globals.accumulator_count);
        }

        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_raytracing_render_target.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        // FINALIZE

        // render imgui
        ImGui::End();
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[frame_index], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET));
        auto rtv_descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_rtv_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, g_rtv_descriptor_size);
        cmd_list->OMSetRenderTargets(1, &rtv_descriptor, FALSE, NULL);

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

        // present backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[frame_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        CHECK_RESULT(cmd_list->Close());

        // execute
        g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);

        CHECK_RESULT(g_swapchain->Present(VSYNC, 0));
    }
}
