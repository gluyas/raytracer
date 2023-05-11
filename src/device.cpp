#include "device.h"

namespace Device {

ID3D12Device5* g_device;
IDXGIFactory7* g_dxgi_factory;
IDXGIAdapter4* g_dxgi_adapter;
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

    { // g_dxgi_adaptor
        IDXGIAdapter* adapter;
        CHECK_RESULT(g_dxgi_factory->EnumAdapters(0, &adapter));
        // CHECK_RESULT(g_dxgi_device->GetAdapter(&adapter));
        CHECK_RESULT(adapter->QueryInterface(IID_PPV_ARGS(&g_dxgi_adapter)));
    }

    g_descriptor_size     = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtv_descriptor_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

ID3D12Resource** push_uninitialized_temp_resource(Array<ID3D12Resource*>* temp_resources) {
    if (!temp_resources) temp_resources = &g_temp_resources;
    ID3D12Resource** temp = array_push_uninitialized(temp_resources);
    *temp = NULL;
    return temp;
}

void push_temp_resource(ID3D12Resource* resource, Array<ID3D12Resource*>* temp_resources) {
    if (!temp_resources) temp_resources = &g_temp_resources;
    return array_push(temp_resources, resource);
}

void release_temp_resources(Array<ID3D12Resource*>* temp_resources) {
    if (!temp_resources) temp_resources = &g_temp_resources;
    for (const auto& resource : *temp_resources) {
        if (resource) resource->Release();
    }
    temp_resources->len = 0;
}

} // namespace Device
using namespace Device;

D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(ID3D12DescriptorHeap* cbv_srv_uav_descriptor_heap, UINT offset) {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(cbv_srv_uav_descriptor_heap->GetCPUDescriptorHandleForHeapStart(), offset, g_descriptor_size);
}
D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(ID3D12DescriptorHeap* cbv_srv_uav_descriptor_heap, UINT offset) {
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(cbv_srv_uav_descriptor_heap->GetGPUDescriptorHandleForHeapStart(), offset, g_descriptor_size);
}
D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_uav_descriptor_handle, UINT offset) {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(cbv_srv_uav_descriptor_handle, offset, g_descriptor_size);
}
D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(D3D12_GPU_DESCRIPTOR_HANDLE cbv_srv_uav_descriptor_handle, UINT offset) {
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(cbv_srv_uav_descriptor_handle, offset, g_descriptor_size);
}
D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(DescriptorHandle cbv_srv_uav_descriptor_handle) {
    return get_cpu_descriptor_handle(cbv_srv_uav_descriptor_handle.heap, cbv_srv_uav_descriptor_handle.offset);
}
D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(DescriptorHandle cbv_srv_uav_descriptor_handle) {
    return get_gpu_descriptor_handle(cbv_srv_uav_descriptor_handle.heap, cbv_srv_uav_descriptor_handle.offset);
}

// buffer helpers

//this is copying the create_texture but I want to learn how to use this

ID3D12Resource* create_texture(ID3D12GraphicsCommandList* cmd_list, wchar_t* filepath) {
    ID3D12Resource* texture = NULL;
    //subresources
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    std::unique_ptr<uint8_t[]> ddsData;

    /*
    * if (&geometry.dds_filepath != nullptr) {
    *   CHECK_RESULT(LoadDDSTextureFromFile(g_device, &geometry.dds_filepath, &texture, ddsData, subresources));
    *   free(&geometry.dds_filepath);
    * }
    */

    //create upload heap
    //find upload buffer size https://github.com/microsoft/DirectXTK12/wiki/DDSTextureLoader

    ID3D12Resource* upload;
    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(data.len), D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL,
        IID_PPV_ARGS(&upload)
    ));

    UpdateSubresources(cmd_list, &texture, &upload, 0, 0, static_cast<UINT>(subresources.size()), subresources.data());

    //create default heap
    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        RESOURCE_DESC, D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,
        IID_PPV_ARGS(&texture)
    ));

    //copy to upload heap
    void* upload_dst;
    CHECK_RESULT(upload->Map(0, &CD3DX12_RANGE(0, data.len), &upload_dst));
    memmove(upload_dst, data.ptr, data.len);

    //update subresource

    //CD3DX12_TEXTURE_COPY_LOCATION(&texture);
    cmd_list->CopyTextureRegion(&CD3DX12_TEXTURE_COPY_LOCATION(texture), 0, 0, 0, &CD3DX12_TEXTURE_COPY_LOCATION(upload, *footprint), NULL);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    return texture;
}

//

ID3D12Resource* create_buffer(UINT64 size_in_bytes, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags) {
    ID3D12Resource* buffer;
    if (initial_state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)                  flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (initial_state & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(heap_type), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(size_in_bytes, flags), initial_state,
        NULL,
        IID_PPV_ARGS(&buffer)
    ));
    return buffer;
}

ID3D12Resource* create_upload_buffer(UINT64 size_in_bytes) {
    return create_buffer(size_in_bytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
}

ID3D12Resource* create_upload_buffer(ArrayView<void> data) {
    return create_buffer_and_write_contents(NULL, data, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, D3D12_HEAP_TYPE_UPLOAD);
}

void create_or_check_capacity_of_temp_buffer(ID3D12Resource*** temp_buffer, UINT64 size, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type) {
    if (!temp_buffer) abort();
    if (!*temp_buffer) {
        *temp_buffer = push_uninitialized_temp_resource();
    }
    if (!**temp_buffer) {
        **temp_buffer = create_buffer(size, initial_state, heap_type);
    } else {
        if ((**temp_buffer)->GetDesc().Width < size) abort();
    }
}

ID3D12Resource* create_buffer_and_write_contents(ID3D12GraphicsCommandList* cmd_list, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload_buffer, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags) {
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) {
        // don't use cmd_list or upload_buffer
        ID3D12Resource* buffer = create_buffer(data.len, initial_state, heap_type);
        copy_to_upload_buffer(buffer, data);
        return buffer;
    }

    flags |= (initial_state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* buffer = create_buffer(data.len, D3D12_RESOURCE_STATE_COPY_DEST, heap_type, flags);

    create_or_check_capacity_of_temp_buffer(&upload_buffer, data.len, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    copy_to_upload_buffer(*upload_buffer, data);

    cmd_list->CopyResource(buffer, *upload_buffer);
    if (initial_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST, initial_state));
    }
    return buffer;
}

void copy_to_upload_buffer(ID3D12Resource* dst, ArrayView<void> src) {
    void* dst_ptr;
    CHECK_RESULT(dst->Map(0, &CD3DX12_RANGE(0, src.len), &dst_ptr));
    memmove(dst_ptr, src.ptr, src.len);
    dst->Unmap(0, &CD3DX12_RANGE(0, src.len));
}

void copy_from_readback_buffer(ArrayView<void> dst, ID3D12Resource* src) {
    void* src_ptr;
    CHECK_RESULT(src->Map(0, &CD3DX12_RANGE(0, dst.len), &src_ptr));
    memmove(dst.ptr, src_ptr, dst.len);
    src->Unmap(0, &CD3DX12_RANGE(0, 0));
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

// texture helpers

ID3D12Resource* create_texture_and_write_contents(ID3D12GraphicsCommandList* cmd_list, D3D12_RESOURCE_DIMENSION dimension, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprint, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload_buffer, D3D12_RESOURCE_FLAGS flags) {
    ID3D12Resource* texture = NULL;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = dimension;
    desc.Format           = footprint->Footprint.Format;
    desc.Width            = footprint->Footprint.Width;
    desc.Height           = footprint->Footprint.Height;
    desc.DepthOrArraySize = footprint->Footprint.Depth;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // TODO: support multiple mip levels
    desc.MipLevels          = 1;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;

    CHECK_RESULT(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,
        IID_PPV_ARGS(&texture)
    ));
    create_or_check_capacity_of_temp_buffer(&upload_buffer, data.len, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);

    copy_to_upload_buffer(*upload_buffer, data);

    // copy to default resource
    D3D12_TEXTURE_COPY_LOCATION dst = CD3DX12_TEXTURE_COPY_LOCATION(texture);
    D3D12_TEXTURE_COPY_LOCATION src = CD3DX12_TEXTURE_COPY_LOCATION(*upload_buffer, *footprint);

    cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, initial_state));
    return texture;
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

// RootArgument constructors

RootArgument RootArgument::cbv(D3D12_GPU_VIRTUAL_ADDRESS cbv)                             { RootArgument r; r.tag = RootArgument::Tag::Cbv;             r._cbv              = cbv;              return r; }
RootArgument RootArgument::srv(D3D12_GPU_VIRTUAL_ADDRESS srv)                             { RootArgument r; r.tag = RootArgument::Tag::Srv;             r._srv              = srv;              return r; }
RootArgument RootArgument::uav(D3D12_GPU_VIRTUAL_ADDRESS uav)                             { RootArgument r; r.tag = RootArgument::Tag::Uav;             r._uav              = uav;              return r; }
RootArgument RootArgument::consts(ArrayView<void> consts)                                 { RootArgument r; r.tag = RootArgument::Tag::Consts;          r._consts           = consts;           return r; }
RootArgument RootArgument::descriptor_table(D3D12_GPU_DESCRIPTOR_HANDLE descriptor_table) { RootArgument r; r.tag = RootArgument::Tag::DescriptorTable; r._descriptor_table = descriptor_table; return r; }

void RootArgument::set_on_command_list(ID3D12GraphicsCommandList* cmd_list, ArrayView<RootArgument> root_args) {
    for (UINT i = 0; i < root_args.len; i++) {
        RootArgument root_arg = root_args[i];

        switch (root_arg.tag) {
            case RootArgument::Tag::NotSet: {
                continue;
            } break;

            case RootArgument::Tag::Cbv: {
                cmd_list->SetComputeRootConstantBufferView(i, root_arg._cbv);
            } break;

            case RootArgument::Tag::Srv: {
                cmd_list->SetComputeRootShaderResourceView(i, root_arg._srv);
            } break;

            case RootArgument::Tag::Uav: {
                cmd_list->SetComputeRootUnorderedAccessView(i, root_arg._uav);
            } break;

            case RootArgument::Tag::Consts: {
                ArrayView<UINT32>* consts = &((ArrayView<UINT32>) root_arg._consts);
                cmd_list->SetComputeRoot32BitConstants(i, consts->len, (void*) consts->ptr, 0);
            } break;

            case RootArgument::Tag::DescriptorTable: {
                cmd_list->SetComputeRootDescriptorTable(i, root_arg._descriptor_table);
            } break;

            default: {
                abort();
            } break;
        }
    }
}

// DEPRECATED

// resource binding helpers
void set_descriptor_table_on_heap_(ID3D12DescriptorHeap* descriptor_heap, DescriptorTable_* dt) {
    D3D12_CPU_DESCRIPTOR_HANDLE dt_base = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
        dt->offset_into_heap * g_descriptor_size
    );

    for (UINT i = 0; i < dt->descriptors.len; i++) {
        Descriptor_* descriptor = dt->descriptors[i];
        if (!descriptor || !*descriptor) continue;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(dt_base, i*g_descriptor_size);

        switch (descriptor->tag) {
            case Descriptor_::Tag::CbvDescriptor: {
                D3D12_CONSTANT_BUFFER_VIEW_DESC* desc = &descriptor->cbv_desc();
                desc->BufferLocation = descriptor->resource->GetGPUVirtualAddress(); // update to match buffer
                g_device->CreateConstantBufferView(desc, handle);
            } break;

            case Descriptor_::Tag::SrvDescriptor: {
                D3D12_SHADER_RESOURCE_VIEW_DESC* desc = descriptor->use_default_desc? NULL : &descriptor->srv_desc();
                g_device->CreateShaderResourceView(descriptor->resource, desc, handle);
            } break;

            case Descriptor_::Tag::UavDescriptor: {
                D3D12_UNORDERED_ACCESS_VIEW_DESC* desc = descriptor->use_default_desc? NULL : &descriptor->uav_desc();
                g_device->CreateUnorderedAccessView(descriptor->resource, NULL, desc, handle);
            } break;

            default: {
                abort();
            } break;
        }
    }
}

void set_root_arguments_(ID3D12GraphicsCommandList* cmd_list, ID3D12DescriptorHeap* descriptor_heap, ArrayView<RootArgument_> args) {
    for (UINT i = 0; i < args.len; i++) {
        RootArgument_ arg = args[i];
        if (!arg) continue;

        switch (arg.tag) {
            case RootArgument_::Tag::RootCbv: {
                cmd_list->SetComputeRootConstantBufferView(i, (*arg._cbv)->GetGPUVirtualAddress());
            } break;

            case RootArgument_::Tag::RootSrv: {
                cmd_list->SetComputeRootShaderResourceView(i, (*arg._srv)->GetGPUVirtualAddress());
            } break;

            case RootArgument_::Tag::RootUav: {
                cmd_list->SetComputeRootUnorderedAccessView(i, (*arg._uav)->GetGPUVirtualAddress());
            } break;

            case RootArgument_::Tag::RootConsts: {
                ArrayView<UINT32>* consts = &((ArrayView<UINT32>) arg._consts);
                cmd_list->SetComputeRoot32BitConstants(i, consts->len, (void*) consts->ptr, 0);
            } break;

            case RootArgument_::Tag::RootDescriptorTable: {
                DescriptorTable_* dt = arg._descriptor_table;
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

// Descriptor methods

// constructors
Descriptor_ Descriptor_::cbv(ID3D12Resource* resource, D3D12_CONSTANT_BUFFER_VIEW_DESC desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor_ descriptor = { name, resource, Descriptor_::Tag::CbvDescriptor, false };
    descriptor._cbv_desc = desc;
    return descriptor;
}
Descriptor_ Descriptor_::srv(ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC  desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor_ descriptor = { name, resource, Descriptor_::Tag::SrvDescriptor, false };
    descriptor._srv_desc = desc;
    return descriptor;
}
Descriptor_ Descriptor_::uav(ID3D12Resource* resource, D3D12_UNORDERED_ACCESS_VIEW_DESC desc, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    Descriptor_ descriptor = { name, resource, Descriptor_::Tag::UavDescriptor, false };
    descriptor._uav_desc = desc;
    return descriptor;
}

Descriptor_ Descriptor_::srv_with_default_desc(ID3D12Resource* resource, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    return Descriptor_ { name, resource, Descriptor_::Tag::SrvDescriptor, true };
}
Descriptor_ Descriptor_::uav_with_default_desc(ID3D12Resource* resource, LPCWSTR name) {
    if (name && resource) resource->SetName(name);
    return Descriptor_ { name, resource, Descriptor_::Tag::UavDescriptor, true };
}

// getters
D3D12_CONSTANT_BUFFER_VIEW_DESC&  Descriptor_::cbv_desc() {
    if (this->tag != Descriptor_::Tag::CbvDescriptor || this->use_default_desc) abort();
    return this->_cbv_desc;
}
D3D12_SHADER_RESOURCE_VIEW_DESC&  Descriptor_::srv_desc() {
    if (this->tag != Descriptor_::Tag::SrvDescriptor || this->use_default_desc) abort();
    return this->_srv_desc;
}
D3D12_UNORDERED_ACCESS_VIEW_DESC& Descriptor_::uav_desc() {
    if (this->tag != Descriptor_::Tag::UavDescriptor || this->use_default_desc) abort();
    return this->_uav_desc;
}

// DescriptorTable methods

UINT DescriptorTable_::offset_after(DescriptorTable_* dt) {
    return dt->offset_into_heap + dt->descriptors.len;
}

// RootArgument_ methods

// constructors
RootArgument_ RootArgument_::not_set(LPCWSTR name) {
    return RootArgument_ { name, RootArgument_::Tag::NotSet };
}

RootArgument_ RootArgument_::cbv(ID3D12Resource** cbv, LPCWSTR name) {
    if (name && cbv && *cbv) (*cbv)->SetName(name);
    RootArgument_ arg = { name, RootArgument_::Tag::RootCbv };
    arg._cbv = cbv;
    return arg;
}
RootArgument_ RootArgument_::srv(ID3D12Resource** srv, LPCWSTR name) {
    if (name && srv && *srv) (*srv)->SetName(name);
    RootArgument_ arg = { name, RootArgument_::Tag::RootSrv };
    arg._srv = srv;
    return arg;
}
RootArgument_ RootArgument_::uav(ID3D12Resource** uav, LPCWSTR name) {
    if (name && uav && *uav) (*uav)->SetName(name);
    RootArgument_ arg = { name, RootArgument_::Tag::RootUav };
    arg._uav = uav;
    return arg;
}
RootArgument_ RootArgument_::consts(ArrayView<void> consts, LPCWSTR name) {
    RootArgument_ arg = { name, RootArgument_::Tag::RootConsts };
    arg._consts = consts;
    return arg;
}
RootArgument_ RootArgument_::descriptor_table(DescriptorTable_* descriptor_table, LPCWSTR name) {
    RootArgument_ arg = { name, RootArgument_::Tag::RootDescriptorTable };
    arg._descriptor_table = descriptor_table;
    return arg;
}
