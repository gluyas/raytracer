#include "device.h"

namespace Device {

ID3D12Device5* g_device;
IDXGIFactory7* g_dxgi_factory;
#ifdef DEBUG
ID3D12Debug* g_debug;
#endif // DEBUG

UINT g_descriptor_size;
UINT g_rtv_descriptor_size;

Array<ID3D12Resource*> g_temp_resources = {};

void init() {
#ifdef DEBUG
    // g_debug_controller
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&g_debug)))) {
        g_debug->EnableDebugLayer();
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

    g_descriptor_size     = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtv_descriptor_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void release_temp_resources(Array<ID3D12Resource*>* temp_resources) {
    if (!temp_resources) temp_resources = &g_temp_resources;
    for (const auto& resource : *temp_resources) {
        resource->Release();
    }
    temp_resources->len = 0;
}

} // namespace Device


using namespace Device;

// buffer helpers

ID3D12Resource* create_buffer(UINT64 size_in_bytes, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags) {
    ID3D12Resource* buffer;
    flags |= (initial_state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(heap_type), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(size_in_bytes, flags), initial_state,
        NULL,
        IID_PPV_ARGS(&buffer)
    ));
    return buffer;
}

void create_or_check_capacity_of_temp_buffer(ID3D12Resource*** temp_buffer, UINT64 size, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type) {
    if (!temp_buffer) abort();
    if (!*temp_buffer) {
        *temp_buffer = array_push(&g_temp_resources, (ID3D12Resource*) NULL);
    }
    if (!**temp_buffer) {
        **temp_buffer = create_buffer(size, initial_state, heap_type);
    } else {
        if ((**temp_buffer)->GetDesc().Width < size) abort();
    }
}

ID3D12Resource* create_buffer_and_write_contents(ID3D12GraphicsCommandList* cmd_list, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload_buffer, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags) {
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) {
        ID3D12Resource* buffer = create_buffer(data.len, initial_state, heap_type);
        copy_to_upload_buffer(buffer, data);
        return buffer;
    }

    flags |= (initial_state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* buffer = create_buffer(data.len, D3D12_RESOURCE_STATE_COPY_DEST, heap_type, flags);

    create_or_check_capacity_of_temp_buffer(&upload_buffer, data.len, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    copy_to_upload_buffer(*upload_buffer, data);

    cmd_list->CopyBufferRegion(buffer, 0, *upload_buffer, 0, data.len);
    if (initial_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, initial_state));
    }
    return buffer;
}

void copy_to_upload_buffer(ID3D12Resource* dst, ArrayView<void> src) {
    void* dst_ptr;
    dst->Map(0, NULL, &dst_ptr);
    memmove(dst_ptr, src.ptr, src.len);
    dst->Unmap(0, &CD3DX12_RANGE(0, src.len));
}

void copy_from_readback_buffer(ArrayView<void> dst, ID3D12Resource* src) {
    void* src_ptr;
    src->Map(0, &CD3DX12_RANGE(0, dst.len), &src_ptr);
    memmove(dst.ptr, src_ptr, dst.len);
    src->Unmap(0, NULL);
}

UINT64 copy_from_readback_buffer(Array<void>* dst, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC desc = src->GetDesc();
    copy_from_readback_buffer(array_push_uninitialized(dst, desc.Width), src);
    return desc.Width;
}

void write_to_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ArrayView<void> src, ID3D12Resource** upload_buffer) {
    create_or_check_capacity_of_temp_buffer(&upload_buffer, src.len, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    copy_to_upload_buffer(*upload_buffer, src);

    if (dst_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST));
    }
    cmd_list->CopyBufferRegion(dst, 0, *upload_buffer, 0, src.len);
    if (dst_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state));
    }
}

ID3D12Resource* read_from_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, ID3D12Resource** readback_buffer) {
    UINT64 size_in_bytes = src->GetDesc().Width;
    create_or_check_capacity_of_temp_buffer(&readback_buffer, size_in_bytes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);

    if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE));
    }
    cmd_list->CopyBufferRegion(*readback_buffer, 0, src, 0, size_in_bytes);
    if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state));
    }
    return *readback_buffer;
}

// root argument helpers

void set_descriptor_table_on_heap(ID3D12DescriptorHeap* descriptor_heap, DescriptorTable* dt) {
    D3D12_CPU_DESCRIPTOR_HANDLE dt_base = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
        dt->offset_into_heap * g_descriptor_size
    );

    for (UINT i = 0; i < dt->descriptors.len; i++) {
        Descriptor* descriptor = dt->descriptors[i];
        if (!descriptor) continue;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(dt_base, i*g_descriptor_size);

        switch (descriptor->tag) {
            case Descriptor::Tag::CbvDescriptor: {
                D3D12_CONSTANT_BUFFER_VIEW_DESC* desc = &descriptor->cbv_desc();
                desc->BufferLocation = descriptor->resource->GetGPUVirtualAddress(); // update to match buffer
                g_device->CreateConstantBufferView(desc, handle);
            } break;

            case Descriptor::Tag::SrvDescriptor: {
                g_device->CreateShaderResourceView(descriptor->resource, &descriptor->srv_desc(), handle);
            } break;

            case Descriptor::Tag::UavDescriptor: {
                g_device->CreateUnorderedAccessView(descriptor->resource, NULL, &descriptor->uav_desc(), handle);
            } break;

            default: {
                abort();
            } break;
        }
    }
}

void set_root_arguments(ID3D12GraphicsCommandList* cmd_list, ID3D12DescriptorHeap* descriptor_heap, ArrayView<RootArgument*> args) {
    for (UINT i = 0; i < args.len; i++) {
        RootArgument* arg = args[i];
        if (!arg) continue;

        switch (arg->tag) {
            case RootArgument::Tag::RootCbv: {
                cmd_list->SetComputeRootConstantBufferView(i, arg->cbv()->GetGPUVirtualAddress());
            } break;

            case RootArgument::Tag::RootSrv: {
                cmd_list->SetComputeRootShaderResourceView(i, arg->srv()->GetGPUVirtualAddress());
            } break;

            case RootArgument::Tag::RootUav: {
                cmd_list->SetComputeRootUnorderedAccessView(i, arg->uav()->GetGPUVirtualAddress());
            } break;

            case RootArgument::Tag::RootConsts: {
                ArrayView<UINT32>* consts = &((ArrayView<UINT32>) arg->consts());
                cmd_list->SetComputeRoot32BitConstants(i, consts->len, (void*) consts->ptr, 0);
            } break;

            case RootArgument::Tag::RootDescriptorTable: {
                DescriptorTable* dt = arg->descriptor_table();
                D3D12_GPU_DESCRIPTOR_HANDLE dt_base = CD3DX12_GPU_DESCRIPTOR_HANDLE(
                    descriptor_heap->GetGPUDescriptorHandleForHeapStart(),
                    dt->offset_into_heap * g_descriptor_size
                );
                cmd_list->SetComputeRootDescriptorTable(i, dt_base);
            } break;

            default: {
                abort();
            } break;
        }
    }
}

// fence methods

Fence Fence::make(UINT64 initial_value) {
    Fence fence = {};
    fence.value = initial_value;
    CHECK_RESULT(g_device->CreateFence(fence.value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.fence)));
    fence.event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!fence.event) {
        CHECK_RESULT(HRESULT_FROM_WIN32(GetLastError()));
    }
    return fence;
}

void Fence::increment_and_signal(ID3D12CommandQueue* cmd_queue, Fence* fence) {
    fence->value += 1;
    CHECK_RESULT(cmd_queue->Signal(fence->fence, fence->value));
}

void Fence::increment_and_signal_and_wait(ID3D12CommandQueue* cmd_queue, Fence* fence) {
    Fence::increment_and_signal(cmd_queue, fence);
    Fence::wait(cmd_queue, fence);
}

void Fence::wait(ID3D12CommandQueue* cmd_queue, Fence* fence) {
    Fence::wait(cmd_queue, fence, fence->value);
}

void Fence::wait(ID3D12CommandQueue* cmd_queue, Fence* fence, UINT64 value) {
    if (fence->fence->GetCompletedValue() < value) {
        CHECK_RESULT(fence->fence->SetEventOnCompletion(value, fence->event));
        WaitForSingleObject(fence->event, INFINITE);
    }
}

// Device::Descriptor methods

// constructors
Descriptor Descriptor::make_cbv(ID3D12Resource* resource, D3D12_CONSTANT_BUFFER_VIEW_DESC desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor descriptor = { name, resource, Descriptor::Tag::CbvDescriptor };
    descriptor._cbv_desc = desc;
    return descriptor;
}
Descriptor Descriptor::make_srv(ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC  desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor descriptor = { name, resource, Descriptor::Tag::SrvDescriptor };
    descriptor._srv_desc = desc;
    return descriptor;
}
Descriptor Descriptor::make_uav(ID3D12Resource* resource, D3D12_UNORDERED_ACCESS_VIEW_DESC desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor descriptor = { name, resource, Descriptor::Tag::UavDescriptor };
    descriptor._uav_desc = desc;
    return descriptor;
}

// getters
D3D12_CONSTANT_BUFFER_VIEW_DESC&  Descriptor::cbv_desc() {
    if (this->tag != Descriptor::Tag::CbvDescriptor) abort();
    return this->_cbv_desc;
}
D3D12_SHADER_RESOURCE_VIEW_DESC&  Descriptor::srv_desc() {
    if (this->tag != Descriptor::Tag::SrvDescriptor) abort();
    return this->_srv_desc;
}
D3D12_UNORDERED_ACCESS_VIEW_DESC& Descriptor::uav_desc() {
    if (this->tag != Descriptor::Tag::UavDescriptor) abort();
    return this->_uav_desc;
}

// Device::RootArgument methods

// constructors
RootArgument RootArgument::make_cbv(ID3D12Resource* cbv, LPCWSTR name) {
    if (name && cbv) cbv->SetName(name);
    RootArgument arg = { name, RootArgument::Tag::RootCbv };
    arg._cbv = cbv;
    return arg;
}
RootArgument RootArgument::make_srv(ID3D12Resource* srv, LPCWSTR name) {
    if (name && srv) srv->SetName(name);
    RootArgument arg = { name, RootArgument::Tag::RootSrv };
    arg._srv = srv;
    return arg;
}
RootArgument RootArgument::make_uav(ID3D12Resource* uav, LPCWSTR name) {
    if (name && uav) uav->SetName(name);
    RootArgument arg = { name, RootArgument::Tag::RootUav };
    arg._uav = uav;
    return arg;
}
RootArgument RootArgument::make_consts(ArrayView<void> consts, LPCWSTR name) {
    RootArgument arg = { name, RootArgument::Tag::RootConsts };
    arg._consts = consts;
    return arg;
}
RootArgument RootArgument::make_descriptor_table(DescriptorTable* descriptor_table, LPCWSTR name) {
    RootArgument arg = { name, RootArgument::Tag::RootDescriptorTable };
    arg._descriptor_table = descriptor_table;
    return arg;
}

// getters
ID3D12Resource*& RootArgument::cbv() {
    if (this->tag != RootArgument::Tag::RootCbv) abort();
    return this->_cbv;
}
ID3D12Resource*& RootArgument::srv() {
    if (this->tag != RootArgument::Tag::RootSrv) abort();
    return this->_srv;
}
ID3D12Resource*& RootArgument::uav() {
    if (this->tag != RootArgument::Tag::RootUav) abort();
    return this->_uav;
}
ArrayView<void>& RootArgument::consts() {
    if (this->tag != RootArgument::Tag::RootConsts) abort();
    return this->_consts;
}
DescriptorTable*& RootArgument::descriptor_table() {
    if (this->tag != RootArgument::Tag::RootDescriptorTable) abort();
    return this->_descriptor_table;
}
