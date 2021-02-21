#include <stdlib.h>

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#ifdef DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif
#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

#include "d3dx12.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "types.h"
using namespace DirectX;

#include "array.h"

#include "parse_obj.h"

#include "out/raytracing.hlsl.h"

// TODO: nicer error handling
#define CHECK_RESULT(hresult) if ((hresult) != S_OK) abort()
#define SET_NAME(object) CHECK_RESULT(object->SetName(L#object))

#define VSYNC 0
#define PIXEL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

#define SWAPCHAIN_BUFFER_COUNT 2

// TODO: move to same header as HLSL root signature definition
#define IMGUI_DESCRIPTOR_INDEX 0
#define RT_DESCRIPTOR_INDEX 1
#define SAMPLE_ACCUMULATOR_DESCRIPTOR_INDEX (RT_DESCRIPTOR_INDEX + 1)
#define VB_DESCRIPTOR_INDEX 3
#define IB_DESCRIPTOR_INDEX (VB_DESCRIPTOR_INDEX + 1)
#define DESCRIPTORS_COUNT 5

// GLOBAL STATE

HWND g_hwnd = NULL;

UINT  g_width;
UINT  g_height;
float g_aspect;

#ifdef DEBUG
ID3D12Debug* g_debug_controller = NULL;
#endif
IDXGIFactory7* g_dxgi_factory = NULL;
ID3D12CommandQueue* g_cmd_queue = NULL;
ID3D12Device5* g_device = NULL;
IDXGISwapChain4* g_swapchain = NULL;
ID3D12DescriptorHeap* g_rtv_descriptor_heap = NULL;
UINT g_rtv_descriptor_size = 0;
ID3D12Resource* g_rtvs[SWAPCHAIN_BUFFER_COUNT] = {};
UINT64 g_fence_value = 0;
ID3D12Fence* g_fence = NULL;
HANDLE g_fence_event = NULL;

ID3D12DescriptorHeap* g_descriptor_heap = NULL;
UINT g_descriptor_size = 0;

ID3D12Resource* g_raytracing_render_target      = NULL;
ID3D12Resource* g_raytracing_sample_accumulator = NULL;

bool g_do_update_resolution = true;
bool g_do_update_camera = true;

// UTILITY FUNCTIONS

void update_resolution() {
    { // get client area
        RECT rect;
        GetClientRect(g_hwnd, &rect);
        g_width  = rect.right  - rect.left;
        g_height = rect.bottom - rect.top;
        g_aspect = (float) g_width / (float) g_height;
        g_do_update_camera = true;
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
        CHECK_RESULT(g_dxgi_factory->CreateSwapChainForHwnd(
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

    { // g_raytracing_render_target, g_raytracing_sample_accumulator
        if (g_raytracing_render_target)      g_raytracing_render_target->Release();
        if (g_raytracing_sample_accumulator) g_raytracing_sample_accumulator->Release();

        // render target
        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Format             = PIXEL_FORMAT;
        // HACK: get actual width / height values
        resource_desc.Width              = g_width;
        resource_desc.Height             = g_height;
        resource_desc.DepthOrArraySize   = 1;
        resource_desc.MipLevels          = 1;
        resource_desc.SampleDesc.Count   = 1;
        resource_desc.SampleDesc.Quality = 0;
        resource_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CHECK_RESULT(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&g_raytracing_render_target)
        ));
        SET_NAME(g_raytracing_render_target);

        auto render_target_descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), RT_DESCRIPTOR_INDEX, g_descriptor_size);
        g_device->CreateUnorderedAccessView(g_raytracing_render_target, NULL, NULL, render_target_descriptor);

        // sample accumulator
        resource_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

        CHECK_RESULT(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&g_raytracing_sample_accumulator)
        ));
        SET_NAME(g_raytracing_sample_accumulator);

        auto sample_accumulator_descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), SAMPLE_ACCUMULATOR_DESCRIPTOR_INDEX, g_descriptor_size);
        g_device->CreateUnorderedAccessView(g_raytracing_sample_accumulator, NULL, NULL, sample_accumulator_descriptor);
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

        default: {
            return DefWindowProc(g_hwnd, msg, wp, lp);
        } break;
    }
}

void set_buffer_contents(ID3D12Resource* buffer, void* data, UINT64 size_in_bytes) {
    if (!data) return;

    void* mapped_ptr;
    CHECK_RESULT(buffer->Map(0, &CD3DX12_RANGE(0, 0), &mapped_ptr));
    {
        memcpy(mapped_ptr, data, size_in_bytes);
    }
    buffer->Unmap(0, NULL);
}

float clamp(float x, float a, float b) {
    return min(max(x, b), a);
}

ID3D12Resource* create_upload_buffer(ID3D12Device* g_device, void* data, UINT64 size_in_bytes) {
    ID3D12Resource* buffer = NULL;
    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(size_in_bytes), D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL,
        IID_PPV_ARGS(&buffer)
    ));
    set_buffer_contents(buffer, data, size_in_bytes);
    return buffer;
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    PWSTR     pCmdLine,
    int       nCmdShow
) {
    // PLATFORM LAYER

    WNDCLASS wc = {};
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WindowProc;
    wc.lpszClassName = L"RaytracerWindowClass";

    RegisterClass(&wc);
    g_hwnd = CreateWindow(
        wc.lpszClassName, L"Raytracer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, hInstance, NULL
    );

    // DEVICE INTERFACE

#ifdef DEBUG
    // g_debug_controller
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&g_debug_controller)))) {
        g_debug_controller->EnableDebugLayer();
    }
#endif

    // g_dxgi_factory
    CHECK_RESULT(CreateDXGIFactory1(IID_PPV_ARGS(&g_dxgi_factory)));

    { // g_device
        CHECK_RESULT(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&g_device)));

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        CHECK_RESULT(g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
        if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
            abort();
        }
    }

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
    g_rtv_descriptor_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // g_fence, g_fence_event
    CHECK_RESULT(g_device->CreateFence(g_fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_fence_event) {
        CHECK_RESULT(HRESULT_FROM_WIN32(GetLastError()));
    }
    g_fence_value = 0;

    { // g_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_descriptor_heap_desc = {};
        g_descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        g_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_descriptor_heap_desc.NumDescriptors = DESCRIPTORS_COUNT;
        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_descriptor_heap_desc, IID_PPV_ARGS(&g_descriptor_heap)));
    }
    g_descriptor_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // IMGUI SETUP

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

    ID3D12CommandAllocator* cmd_allocator = NULL;
    CHECK_RESULT(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)));

    ID3D12GraphicsCommandList4* cmd_list = NULL;
    CHECK_RESULT(g_device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmd_list)));

    // raytracing render target

    ID3D12StateObject*           raytracing_pso        = NULL;
    ID3D12StateObjectProperties* raytracing_properties = NULL;
    {
        auto pso_desc = CD3DX12_STATE_OBJECT_DESC(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

        auto dxil_subobject = pso_desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>(); {
            auto bytecode = CD3DX12_SHADER_BYTECODE((void *) raytracing_hlsl_bytecode, _countof(raytracing_hlsl_bytecode));
            dxil_subobject->SetDXILLibrary(&bytecode);
        }

        // auto lambert_hit_group_subobject = pso_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>(); {
        //     lambert_hit_group_subobject->SetClosestHitShaderImport(L"lambert_chit");
        //     lambert_hit_group_subobject->SetHitGroupExport(L"lambert_hit_group");
        //     lambert_hit_group_subobject->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
        // }

        CHECK_RESULT(g_device->CreateStateObject(pso_desc, IID_PPV_ARGS(&raytracing_pso)));
        CHECK_RESULT(raytracing_pso->QueryInterface(IID_PPV_ARGS(&raytracing_properties)));
    }

    ID3D12RootSignature* raytracing_global_root_signature = NULL;
    CHECK_RESULT(g_device->CreateRootSignature(0, raytracing_hlsl_bytecode, _countof(raytracing_hlsl_bytecode), IID_PPV_ARGS(&raytracing_global_root_signature)));

    // ASSET LOADING

    enum Material {
        Lambert,
        Light,

        Count,
    };

    void* material_hit_group_identifiers[Material::Count] = {}; {
        material_hit_group_identifiers[Lambert] = raytracing_properties->GetShaderIdentifier(L"lambert_hit_group");
        material_hit_group_identifiers[Light]   = raytracing_properties->GetShaderIdentifier(L"light_hit_group");
    }

    struct GeometryInstance {
        const char* filename;
        Material material;
        XMFLOAT3 color;
    };

    // TODO: use actual reflectance values
    // TODO: merge geometries with identical local arguments
    GeometryInstance scene_objects[] = {
        { "data/cornell/floor.obj",     Lambert, { 1, 1, 1 } },
        { "data/cornell/back.obj",      Lambert, { 1, 1, 1 } },
        { "data/cornell/ceiling.obj",   Lambert, { 1, 1, 1 } },
        { "data/cornell/greenwall.obj", Lambert, { 0, 1, 0 } },
        { "data/cornell/redwall.obj",   Lambert, { 1, 0, 0 } },
        { "data/cornell/largebox.obj",  Lambert, { 1, 1, 1 } },
        { "data/cornell/smallbox.obj",  Lambert, { 1, 1, 1 } },

        { "data/cornell/luminaire.obj", Light, { 25, 25, 25 } },
    };

    __declspec(align(32)) struct ShaderRecord {
        char ident[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
        RaytracingLocals locals;
    };

    ID3D12Resource* rgen_shader_table      = NULL;
    ID3D12Resource* hit_group_shader_table = NULL;
    ID3D12Resource* miss_shader_table      = NULL;
    ID3D12Resource* blas_buffer = NULL;
    ID3D12Resource* tlas_buffer = NULL;
    {
        // TODO: factor this procedure somehow
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_descs[_countof(scene_objects)] = {};

        // parse meshes into a single pair of index and vertex buffers
        Array<Vertex> vb_data = {};
        Array<Index>  ib_data = {};
        for (int i = 0; i < _countof(scene_objects); i++) {
            // record offset into the ib array - combine with virtual address after geometry upload
            Index ib_offset = ib_data.len;
            geometry_descs[i].Triangles.IndexBuffer = ib_offset * sizeof(Index);

            // append mesh data to shared ib and vb
            parse_obj_file(scene_objects[i].filename, &vb_data, &ib_data);

            // record number of indices
            geometry_descs[i].Triangles.IndexCount  = ib_data.len - ib_offset;
        }
        // pad index buffer
        while (ib_data.len % 4 != 0) array_push(&ib_data, (Index) 0);

        // build shader tables
        // these need access to the index buffer offsets
        rgen_shader_table = create_upload_buffer(g_device, raytracing_properties->GetShaderIdentifier(L"rgen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        SET_NAME(rgen_shader_table);

        hit_group_shader_table = NULL; {
            ShaderRecord shader_table[_countof(scene_objects)] = {};
            for (int i = 0; i < _countof(scene_objects); i++) {
                void* hit_group_ident = material_hit_group_identifiers[scene_objects[i].material];
                memcpy(shader_table[i].ident, hit_group_ident, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                shader_table[i].locals.color = scene_objects[i].color;
                shader_table[i].locals.primitive_index_offset = geometry_descs[i].Triangles.IndexBuffer / sizeof(Index) / 3;
            }

            hit_group_shader_table = create_upload_buffer(g_device, shader_table, sizeof(shader_table));
            SET_NAME(hit_group_shader_table);
        }

        miss_shader_table = create_upload_buffer(g_device, raytracing_properties->GetShaderIdentifier(L"miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        SET_NAME(miss_shader_table);

        // upload geometry and create SRVs
        ID3D12Resource* vb = create_upload_buffer(g_device, vb_data.ptr, vb_data.len*sizeof(Vertex)); {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format                  = DXGI_FORMAT_UNKNOWN;
            srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            srv_desc.Buffer.NumElements         = vb_data.len;
            srv_desc.Buffer.StructureByteStride = sizeof(Vertex);

            g_device->CreateShaderResourceView(vb, &srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), VB_DESCRIPTOR_INDEX, g_descriptor_size));
            SET_NAME(vb);
        }
        ID3D12Resource* ib = create_upload_buffer(g_device, ib_data.ptr, ib_data.len*sizeof(Index)); {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
            srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            srv_desc.Buffer.NumElements = ib_data.len*sizeof(Index) / 4;
            srv_desc.Buffer.Flags       = D3D12_BUFFER_SRV_FLAG_RAW;

            g_device->CreateShaderResourceView(ib, &srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(g_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), IB_DESCRIPTOR_INDEX, g_descriptor_size));
            SET_NAME(ib);
        }

        // finalize geometry_descs
        for (int i = 0; i < _countof(geometry_descs); i++) {
            geometry_descs[i].Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry_descs[i].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // disables any-hit shader

            geometry_descs[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometry_descs[i].Triangles.VertexCount  = vb_data.len;
            geometry_descs[i].Triangles.VertexBuffer.StartAddress  = vb->GetGPUVirtualAddress();
            geometry_descs[i].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

            geometry_descs[i].Triangles.IndexFormat  = DXGI_FORMAT_R16_UINT;
            // IndexCount was written in previous loop; add address to the offset stored in IndexBuffer
            geometry_descs[i].Triangles.IndexBuffer += ib->GetGPUVirtualAddress();
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
        buffer_desc.Width = max(blas_prebuild.ScratchDataSizeInBytes, tlas_prebuild.ScratchDataSizeInBytes);
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
            IID_PPV_ARGS(&blas_buffer)
        ));
        SET_NAME(blas_buffer);
        blas_build.DestAccelerationStructureData    = blas_buffer->GetGPUVirtualAddress();
        blas_build.SourceAccelerationStructureData  = NULL;
        blas_build.ScratchAccelerationStructureData = scratch_buffer->GetGPUVirtualAddress();

        // tlas instance descs
        ID3D12Resource* instance_descs_buffer = NULL; {
            D3D12_RAYTRACING_INSTANCE_DESC instance_desc = {};
            instance_desc.Transform[0][0] =-0.001;
            instance_desc.Transform[1][2] = 0.001;
            instance_desc.Transform[2][1] = 0.001;
            instance_desc.Transform[0][3] = 0.27;
            instance_desc.Transform[1][3] =-0.27;
            instance_desc.Transform[2][3] =-0.23;
            instance_desc.InstanceMask = 1;
            instance_desc.AccelerationStructure = blas_buffer->GetGPUVirtualAddress();

            instance_descs_buffer = create_upload_buffer(g_device, &instance_desc, sizeof(instance_desc));
            SET_NAME(instance_descs_buffer);
        }

        // tlas build
        tlas_inputs->InstanceDescs = instance_descs_buffer->GetGPUVirtualAddress();

        buffer_desc.Width = tlas_prebuild.ResultDataMaxSizeInBytes;
        CHECK_RESULT(g_device->CreateCommittedResource(
            &buffer_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            NULL,
            IID_PPV_ARGS(&tlas_buffer)
        ));
        SET_NAME(tlas_buffer);
        tlas_build.DestAccelerationStructureData    = tlas_buffer->GetGPUVirtualAddress();
        tlas_build.SourceAccelerationStructureData  = NULL;
        tlas_build.ScratchAccelerationStructureData = scratch_buffer->GetGPUVirtualAddress();

        // execute build
        CHECK_RESULT(cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(cmd_allocator, NULL));
        cmd_list->BuildRaytracingAccelerationStructure(&blas_build, 0, NULL);
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(scratch_buffer));
        cmd_list->BuildRaytracingAccelerationStructure(&tlas_build, 0, NULL);
        CHECK_RESULT(cmd_list->Close());

        ID3D12CommandList* cmd_lists[] = { cmd_list };
        g_cmd_queue->ExecuteCommandLists(1, cmd_lists);

        g_fence_value += 1;
        CHECK_RESULT(g_cmd_queue->Signal(g_fence, g_fence_value));
        if (g_fence->GetCompletedValue() < g_fence_value) {
            CHECK_RESULT(g_fence->SetEventOnCompletion(g_fence_value, g_fence_event));
            WaitForSingleObject(g_fence_event, INFINITE);
        }
        scratch_buffer->Release();
    }

    RaytracingGlobals raytracing_globals = {};
    ID3D12Resource*   raytracing_globals_buffer = create_upload_buffer(g_device, NULL, sizeof(raytracing_globals));

    // MAIN LOOP

    while (true) {
        // SETUP

        // synchronize with g_device
        // TODO: pipeline frames
        g_fence_value += 1;
        CHECK_RESULT(g_cmd_queue->Signal(g_fence, g_fence_value));
        if (g_fence->GetCompletedValue() < g_fence_value) {
            CHECK_RESULT(g_fence->SetEventOnCompletion(g_fence_value, g_fence_event));
            WaitForSingleObject(g_fence_event, INFINITE);
        }

        if (g_do_update_resolution) update_resolution();
        g_do_update_resolution = false;

        // start imgui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // static bool show_demo_window = true; if (show_demo_window) ImGui::ShowDemoWindow(&show_demo_window);
        ImGui::Begin("debug menu");

        // event handler
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // UPDATE

        { // camera
            ImGui::Text("camera");

            static float azimuth   = 0;
            static float elevation = 0;
            static float distance  = 1;
            static float fov_x     = TAU/4;
            static float fov_y     = fov_x / g_aspect;

            g_do_update_camera |= ImGui::SliderAngle("azimuth",   &azimuth);
            azimuth = fmod(azimuth + 3*TAU/2, TAU) - TAU/2;

            g_do_update_camera |= ImGui::SliderAngle("elevation", &elevation, -85, 85);
            g_do_update_camera |= ImGui::DragFloat("distance", &distance, 0.1, 0.1, FLT_MAX);

            if (ImGui::SliderAngle("fov x", &fov_x, 5,          175))          { fov_y = fov_x / g_aspect; g_do_update_camera = true; }
            if (ImGui::SliderAngle("fov y", &fov_y, 5/g_aspect, 175/g_aspect)) { fov_x = fov_y * g_aspect; g_do_update_camera = true; }

            XMVECTOR camera_pos = XMVectorSet(
                -sinf(azimuth) * cosf(elevation) * distance,
                -cosf(azimuth) * cosf(elevation) * distance,
                                 sinf(elevation) * distance,
                1
            );
            raytracing_globals.camera_aspect = g_aspect;
            raytracing_globals.camera_focal_length = 1 / tanf(fov_y/2);
            XMMATRIX view = XMMatrixLookAtRH(camera_pos, g_XMZero, g_XMIdentityR2);
            raytracing_globals.camera_to_world = XMMatrixInverse(NULL, view);
        }

        // frame accumulator
        if (g_do_update_camera) raytracing_globals.accumulator_count  = 0;
        else                    raytracing_globals.accumulator_count += 1;
        g_do_update_camera = 0;
        ImGui::Text("accumulated frames: %d", raytracing_globals.accumulator_count);

        // upload frame constants
        set_buffer_contents(raytracing_globals_buffer, &raytracing_globals, sizeof(raytracing_globals));

        // RENDER

        UINT frame_index = g_swapchain->GetCurrentBackBufferIndex();

        CHECK_RESULT(cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(cmd_allocator, NULL));

        cmd_list->SetDescriptorHeaps(1, &g_descriptor_heap);

        // bind global root arguments
        // TODO: less error prone way of determining binding slots
        cmd_list->SetComputeRootSignature(raytracing_global_root_signature); {
            D3D12_GPU_DESCRIPTOR_HANDLE heap_base = g_descriptor_heap->GetGPUDescriptorHandleForHeapStart();

            auto render_target_descriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(heap_base, RT_DESCRIPTOR_INDEX, g_descriptor_size);
            cmd_list->SetComputeRootDescriptorTable(0, render_target_descriptor);

            cmd_list->SetComputeRootConstantBufferView(1, raytracing_globals_buffer->GetGPUVirtualAddress());

            cmd_list->SetComputeRootShaderResourceView(2, tlas_buffer->GetGPUVirtualAddress());

            auto vb_ib_descriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(heap_base, VB_DESCRIPTOR_INDEX, g_descriptor_size);
            cmd_list->SetComputeRootDescriptorTable(3, vb_ib_descriptor);
        }

        // raytracing
        D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
        dispatch_rays.HitGroupTable.StartAddress  = hit_group_shader_table->GetGPUVirtualAddress();
        dispatch_rays.HitGroupTable.SizeInBytes   = hit_group_shader_table->GetDesc().Width;
        dispatch_rays.HitGroupTable.StrideInBytes = sizeof(ShaderRecord);

        dispatch_rays.MissShaderTable.StartAddress  = miss_shader_table->GetGPUVirtualAddress();
        dispatch_rays.MissShaderTable.SizeInBytes   = miss_shader_table->GetDesc().Width;
        dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // TODO: calculate this

        dispatch_rays.RayGenerationShaderRecord.StartAddress = rgen_shader_table->GetGPUVirtualAddress();
        dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = rgen_shader_table->GetDesc().Width;

        dispatch_rays.Width  = g_width;
        dispatch_rays.Height = g_height;
        dispatch_rays.Depth  = 1;

        cmd_list->SetPipelineState1(raytracing_pso);
        cmd_list->DispatchRays(&dispatch_rays);
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(g_raytracing_render_target));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(g_raytracing_sample_accumulator));

        // copy raytracing output to backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_raytracing_render_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_rtvs[frame_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

        cmd_list->CopyResource(g_rtvs[frame_index], g_raytracing_render_target);

        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_raytracing_render_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

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
        ID3D12CommandList* cmd_lists[] = { cmd_list };
        g_cmd_queue->ExecuteCommandLists(1, cmd_lists);

        CHECK_RESULT(g_swapchain->Present(VSYNC, 0));
    }
}
