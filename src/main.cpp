#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#ifdef _DEBUG
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

// TODO: nicer error handling
#define CHECK_RESULT(hresult) if ((hresult) != S_OK) abort()

#define WIDTH 1280
#define HEIGHT 720

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

#if defined(_DEBUG)
    ID3D12Debug* debug_controller = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
        debug_controller->EnableDebugLayer();
    }
#endif

    IDXGIFactory7* dxgi_factory = NULL;
    CHECK_RESULT(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)));

    ID3D12Device5* device = NULL;
    CHECK_RESULT(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

    ID3D12CommandQueue* cmd_queue = NULL; {
        D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
        cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        cmd_queue_desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;

        CHECK_RESULT(device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&cmd_queue)));
    }

    IDXGISwapChain4* swapchain = NULL; {
        DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
        swapchain_desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
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
            device->CreateRenderTargetView(rtvs[n], nullptr, rtv_handle);
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

    ID3D12DescriptorHeap* descriptor_heap = NULL; {
        D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
        descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descriptor_heap_desc.NumDescriptors = 1; // TODO: less error-prone way of allocating descriptors?
        CHECK_RESULT(device->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap)));
    }

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
        descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
        descriptor_heap->GetGPUDescriptorHandleForHeapStart()
    );

    // RENDER PIPELINE

    // empty root signature for basic shaders
    ID3D12RootSignature* root_signature = NULL; {
        D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
        root_signature_desc.NumParameters     = 0;
        root_signature_desc.NumStaticSamplers = 0;
        root_signature_desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* signature = NULL;
        ID3DBlob* error     = NULL;
        CHECK_RESULT(D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        CHECK_RESULT(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)));
    }

    // initial state and shader setup
    ID3D12PipelineState *pso = NULL; {
        ID3DBlob* vertex_shader = NULL;
        ID3DBlob* pixel_shader  = NULL;

#if defined(_DEBUG)
        UINT compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION; // improves debugging
#else
        UINT compile_flags = 0;
#endif

        CHECK_RESULT(D3DCompileFromFile(L"./src/basic.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, nullptr));
        CHECK_RESULT(D3DCompileFromFile(L"./src/basic.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compile_flags, 0, &pixel_shader,  nullptr));

        D3D12_INPUT_ELEMENT_DESC input_element_descs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.InputLayout    = { input_element_descs, _countof(input_element_descs) };
        pso_desc.pRootSignature = root_signature;
        pso_desc.VS = { reinterpret_cast<UINT8*>(vertex_shader->GetBufferPointer()), vertex_shader->GetBufferSize() };
        pso_desc.PS = { reinterpret_cast<UINT8*>( pixel_shader->GetBufferPointer()),  pixel_shader->GetBufferSize() };

        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.SampleDesc.Count   = 1;
        pso_desc.SampleDesc.Quality = 0;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso_desc.BlendState      = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso_desc.DepthStencilState.DepthEnable   = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;

        CHECK_RESULT(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
    }

    ID3D12CommandAllocator* cmd_allocator = NULL;
    CHECK_RESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)));

    ID3D12GraphicsCommandList4* cmd_list = NULL;
    CHECK_RESULT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator, pso, IID_PPV_ARGS(&cmd_list)));
    CHECK_RESULT(cmd_list->Close()); // render loop expects closed command list

    // ASSET LOADING

    ID3D12Resource*          vb  = NULL;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    {
        Vertex vb_data[] = {
            { { 0.0,  0.25, 0.0 }, { 1.0, 0.0, 0.0 } },
            { { 0.25,-0.25, 0.0 }, { 0.0, 1.0, 0.0 } },
            { {-0.25,-0.25, 0.0 }, { 0.0, 0.0, 1.0 } }
        };
        UINT vb_size = sizeof(vb_data);

        // create an upload heap for the geometry
        // TODO: use default heap to improve performance
        auto resource_desc   = CD3DX12_RESOURCE_DESC::Buffer(vb_size);
        auto heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        CHECK_RESULT(device->CreateCommittedResource(
            &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vb)
        ));

        // copy vertex data
        UINT8* mapped_ptr;
        auto empty_range = CD3DX12_RANGE(0, 0);
        CHECK_RESULT(vb->Map(0, &empty_range, reinterpret_cast<void**>(&mapped_ptr))); {
            memcpy(mapped_ptr, vb_data, vb_size);
        } vb->Unmap(0, nullptr);

        vbv.BufferLocation = vb->GetGPUVirtualAddress();
        vbv.StrideInBytes  = sizeof(Vertex);
        vbv.SizeInBytes    = vb_size;
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
        static bool show_demo_window = true; ImGui::ShowDemoWindow(&show_demo_window);

        // handle input
        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // RENDER FRAME

        UINT frame_index = swapchain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER barrier;

        CHECK_RESULT(cmd_allocator->Reset());
        CHECK_RESULT(cmd_list->Reset(cmd_allocator, pso));

        cmd_list->SetGraphicsRootSignature(root_signature);
        cmd_list->SetDescriptorHeaps(1, &descriptor_heap);

        D3D12_VIEWPORT viewport = { 0, 0, WIDTH, HEIGHT };
        cmd_list->RSSetViewports(1, &viewport);
        D3D12_RECT scissor_rect = { 0, 0, WIDTH, HEIGHT };
        cmd_list->RSSetScissorRects(1, &scissor_rect);

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(rtvs[frame_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd_list->ResourceBarrier(1, &barrier);

        // transition backbuffer for render target
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtv_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, rtv_descriptor_size);
        cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

        // draw triangle
        const float clear[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        cmd_list->ClearRenderTargetView(rtv_handle, clear, 0, nullptr);
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd_list->IASetVertexBuffers(0, 1, &vbv);
        cmd_list->DrawInstanced(3, 1, 0, 0);

        // render imgui
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(rtvs[frame_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        cmd_list->ResourceBarrier(1, &barrier);
        CHECK_RESULT(cmd_list->Close());

        // execute
        ID3D12CommandList* cmd_lists[] = { cmd_list };
        cmd_queue->ExecuteCommandLists(1, cmd_lists);

        CHECK_RESULT(swapchain->Present(1, 0)); // vsync
    }
}
