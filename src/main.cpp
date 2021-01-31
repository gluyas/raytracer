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

#include "out/raytracing.hlsl.h"

// TODO: nicer error handling
#define CHECK_RESULT(hresult) if ((hresult) != S_OK) abort()
#define SET_NAME(object) CHECK_RESULT(object->SetName(L#object))

// TODO: get viewport dimensions at runtime
#define WIDTH 1280
#define MAGIC_WIDTH (WIDTH - 16)
#define HEIGHT 720
#define MAGIC_HEIGHT ( HEIGHT - 39 )
#define PIXEL_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

#define SWAPCHAIN_BUFFER_COUNT 2

LRESULT CALLBACK WindowProc(
    HWND   hwnd,
    UINT   msg,
    WPARAM wp,
    LPARAM lp
) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 0;

    switch (msg) {
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        } break;

        default: {
            return DefWindowProc(hwnd, msg, wp, lp);
        } break;
    }
}

ID3D12Resource* copy_to_upload_buffer(ID3D12Device* device, void* data, UINT size_in_bytes) {
    ID3D12Resource* resource = NULL;
    CHECK_RESULT(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(size_in_bytes), D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL,
        IID_PPV_ARGS(&resource)
    ));

    void* mapped_ptr;
    CHECK_RESULT(resource->Map(0, &CD3DX12_RANGE(0, 0), &mapped_ptr));
    {
        memcpy(mapped_ptr, data, size_in_bytes);
    }
    resource->Unmap(0, NULL);

    return resource;
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
    HWND hwnd = CreateWindow(
        wc.lpszClassName, L"Raytracer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, HEIGHT,
        NULL, NULL, hInstance, NULL
    );

    // DEVICE INTERFACE

#ifdef DEBUG
    ID3D12Debug* debug_controller = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
        debug_controller->EnableDebugLayer();
    }
#endif

    IDXGIFactory7* dxgi_factory = NULL;
    CHECK_RESULT(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));

    ID3D12Device5* device = NULL; {
        CHECK_RESULT(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        CHECK_RESULT(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
        if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
            abort();
        }
    }

    ID3D12CommandQueue* cmd_queue = NULL; {
        D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
        cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        cmd_queue_desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;

        CHECK_RESULT(device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&cmd_queue)));
    }

    IDXGISwapChain4* swapchain = NULL; {
        DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
        swapchain_desc.Format             = PIXEL_FORMAT;
        swapchain_desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapchain_desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchain_desc.BufferCount        = SWAPCHAIN_BUFFER_COUNT;
        swapchain_desc.SampleDesc.Count   = 1;
        swapchain_desc.SampleDesc.Quality = 0;
        swapchain_desc.Scaling            = DXGI_SCALING_NONE;
        swapchain_desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;

        IDXGISwapChain1* swapchain1 = NULL;
        CHECK_RESULT(dxgi_factory->CreateSwapChainForHwnd(
            cmd_queue, hwnd,
            &swapchain_desc, NULL, NULL,
            &swapchain1
        ));
        CHECK_RESULT(swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain)));
    }

    ID3D12DescriptorHeap* rtv_heap = NULL; {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
        rtv_heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;

        CHECK_RESULT(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
    }
    UINT rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ID3D12Resource* rtvs[SWAPCHAIN_BUFFER_COUNT] = {}; {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();

        for (UINT n = 0; n < SWAPCHAIN_BUFFER_COUNT; n++) {
            CHECK_RESULT(swapchain->GetBuffer(n, IID_PPV_ARGS(&rtvs[n])));
            device->CreateRenderTargetView(rtvs[n], NULL, rtv_handle);
            rtv_handle.ptr += rtv_descriptor_size;
        }
    }

    UINT64 fence_value = 0;
    ID3D12Fence* fence = NULL;
    CHECK_RESULT(device->CreateFence(fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!fence_event) {
        CHECK_RESULT(HRESULT_FROM_WIN32(GetLastError()));
    }

// TODO: better way of allocating descriptors
#define IMGUI_DESCRIPTOR_INDEX 0
#define RT_DESCRIPTOR_INDEX 1
#define VB_DESCRIPTOR_INDEX 2
#define IB_DESCRIPTOR_INDEX (VB_DESCRIPTOR_INDEX + 1)
#define DESCRIPTORS_COUNT 4

    ID3D12DescriptorHeap* descriptor_heap = NULL; {
        D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
        descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descriptor_heap_desc.NumDescriptors = DESCRIPTORS_COUNT;
        CHECK_RESULT(device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap)));
    }
    UINT descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // IMGUI SETUP

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(
        device, SWAPCHAIN_BUFFER_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        descriptor_heap,
        CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptor_heap->GetCPUDescriptorHandleForHeapStart(), IMGUI_DESCRIPTOR_INDEX, descriptor_size),
        CD3DX12_GPU_DESCRIPTOR_HANDLE(descriptor_heap->GetGPUDescriptorHandleForHeapStart(), IMGUI_DESCRIPTOR_INDEX, descriptor_size)
    );

    // RAYTRACING PIPELINE

    ID3D12CommandAllocator* cmd_allocator = NULL;
    CHECK_RESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)));

    ID3D12GraphicsCommandList4* cmd_list = NULL;
    CHECK_RESULT(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmd_list)));

    ID3D12Resource* raytracing_render_target = NULL; {
        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Format             = PIXEL_FORMAT;
        // HACK: get actual width / height values
        resource_desc.Width              = MAGIC_WIDTH;
        resource_desc.Height             = MAGIC_HEIGHT;
        resource_desc.DepthOrArraySize   = 1;
        resource_desc.MipLevels          = 1;
        resource_desc.SampleDesc.Count   = 1;
        resource_desc.SampleDesc.Quality = 0;
        resource_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CHECK_RESULT(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&raytracing_render_target)
        ));
        SET_NAME(raytracing_render_target);

        auto descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptor_heap->GetCPUDescriptorHandleForHeapStart(), RT_DESCRIPTOR_INDEX, descriptor_size);
        device->CreateUnorderedAccessView(raytracing_render_target, NULL, NULL, descriptor);
    }

    ID3D12StateObject*           raytracing_pso        = NULL;
    ID3D12StateObjectProperties* raytracing_properties = NULL;
    {
        auto pso_desc = CD3DX12_STATE_OBJECT_DESC(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

        auto dxil_subobject = pso_desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>(); {
            auto bytecode = CD3DX12_SHADER_BYTECODE((void *) raytracing_hlsl_bytecode, _countof(raytracing_hlsl_bytecode));
            dxil_subobject->SetDXILLibrary(&bytecode);
        }

        // auto hit_group_subobject = pso_desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>(); {
        //     hit_group_subobject->SetClosestHitShaderImport(L"chit");
        //     hit_group_subobject->SetHitGroupExport(L"hit_group");
        //     hit_group_subobject->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
        // }

        CHECK_RESULT(device->CreateStateObject(pso_desc, IID_PPV_ARGS(&raytracing_pso)));
        CHECK_RESULT(raytracing_pso->QueryInterface(IID_PPV_ARGS(&raytracing_properties)));
    }

    ID3D12RootSignature* raytracing_global_root_signature = NULL;
    CHECK_RESULT(device->CreateRootSignature(0, raytracing_hlsl_bytecode, _countof(raytracing_hlsl_bytecode), IID_PPV_ARGS(&raytracing_global_root_signature)));

    // build trivial shader tables
    ID3D12Resource* rgen_shader_table = copy_to_upload_buffer(device, raytracing_properties->GetShaderIdentifier(L"rgen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    SET_NAME(rgen_shader_table);

    ID3D12Resource* hit_group_shader_table = copy_to_upload_buffer(device, raytracing_properties->GetShaderIdentifier(L"hit_group"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    SET_NAME(hit_group_shader_table);

    ID3D12Resource* miss_shader_table = copy_to_upload_buffer(device, raytracing_properties->GetShaderIdentifier(L"miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    SET_NAME(miss_shader_table);

    // ASSET LOADING

    ID3D12Resource* blas_buffer = NULL;
    ID3D12Resource* tlas_buffer = NULL;
    {
        Vertex vb_data[] = {
            { { 0.0,  0.25, 0.0 }, { 1.0, 0.0, 0.0 } },
            { { 0.25,-0.25, 0.0 }, { 0.0, 1.0, 0.0 } },
            { {-0.25,-0.25, 0.0 }, { 0.0, 0.0, 1.0 } }
        };
        Index  ib_data[] = {
            0, 1, 2, 0 // TODO: padding necessary?
        };

        // upload geometry and create SRVs
        ID3D12Resource* vb = copy_to_upload_buffer(device, vb_data, sizeof(vb_data)); {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format                  = DXGI_FORMAT_UNKNOWN;
            srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            srv_desc.Buffer.NumElements         = _countof(vb_data);
            srv_desc.Buffer.StructureByteStride = sizeof(Vertex);

            device->CreateShaderResourceView(vb, &srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptor_heap->GetCPUDescriptorHandleForHeapStart(), VB_DESCRIPTOR_INDEX, descriptor_size));
            SET_NAME(vb);
        }
        ID3D12Resource* ib = copy_to_upload_buffer(device, ib_data, sizeof(ib_data)); {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
            srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            srv_desc.Buffer.NumElements = sizeof(ib_data) / 4;
            srv_desc.Buffer.Flags       = D3D12_BUFFER_SRV_FLAG_RAW;

            device->CreateShaderResourceView(ib, &srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptor_heap->GetCPUDescriptorHandleForHeapStart(), IB_DESCRIPTOR_INDEX, descriptor_size));
            SET_NAME(ib);
        }

        // build raytracing geometry
        // TODO: extract into a callable procedure
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build = {};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build = {};

        // triangle mesh
        D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
        geometry.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // disables any-hit shader

        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount  = _countof(vb_data);
        geometry.Triangles.VertexBuffer.StartAddress  = vb->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

        geometry.Triangles.IndexFormat  = DXGI_FORMAT_R16_UINT;
        geometry.Triangles.IndexCount   = _countof(ib_data);
        geometry.Triangles.IndexBuffer  = ib->GetGPUVirtualAddress();

        // blas inputs
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* blas_inputs = &blas_build.Inputs;
        blas_inputs->Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs->Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blas_inputs->DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs->NumDescs       = 1;
        blas_inputs->pGeometryDescs = &geometry;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_prebuild = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(blas_inputs, &blas_prebuild);

        // tlas inputs
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* tlas_inputs = &tlas_build.Inputs;
        tlas_inputs->Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs->Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlas_inputs->DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlas_inputs->NumDescs       = 1;
        tlas_inputs->InstanceDescs  = NULL; // wait until blas is built on device

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_prebuild = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(tlas_inputs, &tlas_prebuild);

        // allocate buffers and build
        auto buffer_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto buffer_desc       = CD3DX12_RESOURCE_DESC::Buffer(0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        // scratch buffer
        buffer_desc.Width = max(blas_prebuild.ScratchDataSizeInBytes, tlas_prebuild.ScratchDataSizeInBytes);
        ID3D12Resource* scratch_buffer = NULL;
        CHECK_RESULT(device->CreateCommittedResource(
            &buffer_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&scratch_buffer)
        ));
        SET_NAME(scratch_buffer);

        // blas build
        buffer_desc.Width = blas_prebuild.ResultDataMaxSizeInBytes;
        CHECK_RESULT(device->CreateCommittedResource(
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
            instance_desc.Transform[0][0] = 1;
            instance_desc.Transform[1][1] = 1;
            instance_desc.Transform[2][2] = 1;
            instance_desc.InstanceMask = 1;
            instance_desc.AccelerationStructure = blas_buffer->GetGPUVirtualAddress();

            instance_descs_buffer = copy_to_upload_buffer(device, &instance_desc, sizeof(instance_desc));
            SET_NAME(instance_descs_buffer);
        }

        // tlas build
        tlas_inputs->InstanceDescs = instance_descs_buffer->GetGPUVirtualAddress();

        buffer_desc.Width = tlas_prebuild.ResultDataMaxSizeInBytes;
        CHECK_RESULT(device->CreateCommittedResource(
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
        cmd_queue->ExecuteCommandLists(1, cmd_lists);

        fence_value += 1;
        CHECK_RESULT(cmd_queue->Signal(fence, fence_value));
        if (fence->GetCompletedValue() < fence_value) {
            CHECK_RESULT(fence->SetEventOnCompletion(fence_value, fence_event));
            WaitForSingleObject(fence_event, INFINITE);
        }
        scratch_buffer->Release();
    }

    // MAIN LOOP

    while (true) {
        // FRAME SETUP

        // synchronize with device
        // TODO: pipeline frames
        fence_value += 1;
        CHECK_RESULT(cmd_queue->Signal(fence, fence_value));
        if (fence->GetCompletedValue() < fence_value) {
            CHECK_RESULT(fence->SetEventOnCompletion(fence_value, fence_event));
            WaitForSingleObject(fence_event, INFINITE);
        }

        // start imgui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        static bool show_demo_window = true; if (show_demo_window) ImGui::ShowDemoWindow(&show_demo_window);

        // event handler
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // RENDER FRAME

        UINT frame_index = swapchain->GetCurrentBackBufferIndex();

        CHECK_RESULT(cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(cmd_allocator, NULL));

        cmd_list->SetDescriptorHeaps(1, &descriptor_heap);

        D3D12_VIEWPORT viewport = { 0, 0, WIDTH, HEIGHT };
        cmd_list->RSSetViewports(1, &viewport);
        D3D12_RECT scissor_rect = { 0, 0, WIDTH, HEIGHT };
        cmd_list->RSSetScissorRects(1, &scissor_rect);

        // bind global root arguments
        // TODO: less error prone way of determining binding slots
        cmd_list->SetComputeRootSignature(raytracing_global_root_signature); {
            D3D12_GPU_DESCRIPTOR_HANDLE heap_base = descriptor_heap->GetGPUDescriptorHandleForHeapStart();

            auto render_target_descriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(heap_base, RT_DESCRIPTOR_INDEX, descriptor_size);
            cmd_list->SetComputeRootDescriptorTable(0, render_target_descriptor);

            cmd_list->SetComputeRootShaderResourceView(1, tlas_buffer->GetGPUVirtualAddress());

            auto vb_ib_descriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(heap_base, VB_DESCRIPTOR_INDEX, descriptor_size);
            cmd_list->SetComputeRootDescriptorTable(2, vb_ib_descriptor);
        }

        // raytracing
        D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
        dispatch_rays.HitGroupTable.StartAddress  = hit_group_shader_table->GetGPUVirtualAddress();
        dispatch_rays.HitGroupTable.SizeInBytes   = hit_group_shader_table->GetDesc().Width;
        dispatch_rays.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // TODO: calculate this

        dispatch_rays.MissShaderTable.StartAddress  = miss_shader_table->GetGPUVirtualAddress();
        dispatch_rays.MissShaderTable.SizeInBytes   = miss_shader_table->GetDesc().Width;
        dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // TODO: calculate this

        dispatch_rays.RayGenerationShaderRecord.StartAddress = rgen_shader_table->GetGPUVirtualAddress();
        dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = rgen_shader_table->GetDesc().Width;

        dispatch_rays.Width  = MAGIC_WIDTH;
        dispatch_rays.Height = MAGIC_HEIGHT;
        dispatch_rays.Depth  = 1;

        cmd_list->SetPipelineState1(raytracing_pso);
        cmd_list->DispatchRays(&dispatch_rays);
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(raytracing_render_target));

        // copy raytracing output to backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(raytracing_render_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(rtvs[frame_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));

        cmd_list->CopyResource(rtvs[frame_index], raytracing_render_target);

        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(raytracing_render_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        // render imgui
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(rtvs[frame_index], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET));
        auto rtv_descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtv_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, rtv_descriptor_size);
        cmd_list->OMSetRenderTargets(1, &rtv_descriptor, FALSE, NULL);

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

        // present backbuffer
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(rtvs[frame_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        CHECK_RESULT(cmd_list->Close());

        // execute
        ID3D12CommandList* cmd_lists[] = { cmd_list };
        cmd_queue->ExecuteCommandLists(1, cmd_lists);

        CHECK_RESULT(swapchain->Present(1, 0)); // vsync
    }
}
