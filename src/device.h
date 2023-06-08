#pragma once
#include <vector>

#include "prelude.h"

struct Fence {
    UINT64       value;
    HANDLE       event;
    ID3D12Fence* fence;

    static Fence make(UINT64 initial_value = 0);
    static void increment_and_signal(ID3D12CommandQueue* cmd_queue, Fence* fence);
    static void increment_and_signal_and_wait(ID3D12CommandQueue* cmd_queue, Fence* fence);
    static void wait(ID3D12CommandQueue* cmd_queue, Fence* fence);
    static void wait(ID3D12CommandQueue* cmd_queue, Fence* fence, UINT64 value);
};

D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(ID3D12DescriptorHeap* cbv_srv_uav_descriptor_heap, UINT offset = 0);
D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(ID3D12DescriptorHeap* cbv_srv_uav_descriptor_heap, UINT offset = 0);
D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_uav_descriptor_handle, UINT offset);
D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(D3D12_GPU_DESCRIPTOR_HANDLE cbv_srv_uav_descriptor_handle, UINT offset);

struct DescriptorHandle {
    ID3D12DescriptorHeap* heap;
    UINT                  offset;

    inline DescriptorHandle operator+(UINT offset) {
        return { this->heap, this->offset + offset };
    }

    inline operator D3D12_CPU_DESCRIPTOR_HANDLE() {
        return get_cpu_descriptor_handle(this->heap, this->offset);
    }

    inline operator D3D12_GPU_DESCRIPTOR_HANDLE() {
        return get_gpu_descriptor_handle(this->heap, this->offset);
    }
};

struct RootArgument {
    enum Tag {
        NotSet = 0,
        Cbv,
        Srv,
        Uav,
        Consts,
        DescriptorTable
    } tag;
    union {
        D3D12_GPU_VIRTUAL_ADDRESS   _cbv;
        D3D12_GPU_VIRTUAL_ADDRESS   _srv;
        D3D12_GPU_VIRTUAL_ADDRESS   _uav;
        D3D12_GPU_DESCRIPTOR_HANDLE _descriptor_table;
        ArrayView<void>             _consts;
    };

    static RootArgument cbv(D3D12_GPU_VIRTUAL_ADDRESS cbv);
    static RootArgument srv(D3D12_GPU_VIRTUAL_ADDRESS srv);
    static RootArgument uav(D3D12_GPU_VIRTUAL_ADDRESS uav);
    static RootArgument consts(ArrayView<void> consts);
    static RootArgument descriptor_table(D3D12_GPU_DESCRIPTOR_HANDLE descriptor_table);

    static void set_on_command_list(ID3D12GraphicsCommandList* cmd_list, ArrayView<RootArgument> args);
};

namespace Device {

extern ID3D12Device5* g_device;

extern IDXGIFactory7* g_dxgi_factory;
extern IDXGIAdapter4* g_dxgi_adapter;

#ifdef DEBUG
extern ID3D12Debug* g_debug;
#endif // DEBUG

extern UINT g_descriptor_size;
extern UINT g_rtv_descriptor_size;

void init();

ID3D12Resource** push_uninitialized_temp_resource(Array<ID3D12Resource*>* temp_resources = NULL);
void push_temp_resource(ID3D12Resource* resource, Array<ID3D12Resource*>* temp_resources = NULL);
void release_temp_resources(Array<ID3D12Resource*>* temp_resources = NULL);

} // namespace Device
ID3D12Resource* create_texture(ID3D12GraphicsCommandList* cmd_list, char* filepath, D3D12_CPU_DESCRIPTOR_HANDLE desc_heap_handle);
ID3D12Resource* create_buffer(UINT64 size_in_bytes, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
ID3D12Resource* create_upload_buffer(UINT64 size_in_bytes);
ID3D12Resource* create_upload_buffer(ArrayView<void> data);
ID3D12Resource* create_buffer_and_write_contents(ID3D12GraphicsCommandList* cmd_list, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload_buffer = NULL, D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

void copy_to_upload_buffer(ID3D12Resource* dst, ArrayView<void> src);
void copy_from_readback_buffer(ArrayView<void> dst, ID3D12Resource* src);
UINT64 copy_from_readback_buffer(Array<void>* dst, ID3D12Resource* src);

void write_to_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ArrayView<void> src, ID3D12Resource** upload_buffer = NULL);
ID3D12Resource* read_from_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, ID3D12Resource** readback_buffer);

ID3D12Resource* create_texture_and_write_contents(ID3D12GraphicsCommandList* cmd_list, D3D12_RESOURCE_DIMENSION dimension, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprint, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload = NULL, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

// mapped buffer arrays

template<typename T>
struct MappedView : ArrayView<T> {
    ID3D12Resource* resource;
    UINT            subresource;
};

template<typename T>
inline void array_map_resource(MappedView<T>* view) {
    if (view->ptr) abort();
    CHECK_RESULT(view->resource->Map(view->subresource, &CD3DX12_RANGE(0, array_len_in_bytes(view)), (void**) &view->ptr));
}

template<typename T>
inline void array_unmap_resource(MappedView<T>* view) {
    view->resource->Unmap(view->subresource, &CD3DX12_RANGE(0, array_len_in_bytes(view)));
    view->ptr = NULL;
}

template<typename T>
inline struct MappedView<T> array_init_device_buffer(UINT64 len, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    MappedView<T> view = {};
    view.len = len;
    view.resource = create_buffer(array_len_in_bytes(&view), initial_state, heap_type, flags);
    view.subresource = 0;
    return view;
}

template<typename T>
inline struct MappedView<T> array_init_mapped_upload_buffer(UINT64 len) {
    MappedView<T> view = array_init_device_buffer<T>(len, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    array_map_resource(&view);
    return view;
}

template<typename T>
inline struct MappedView<T> array_init_unmapped_readback_buffer(UINT64 len) {
    return array_init_device_buffer<T>(len, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
}

template<typename T>
inline struct MappedView<T> array_from_resource(ID3D12Resource* resource, UINT64 len = 0, UINT subresource = 0) {
    MappedView<T> view = {};
    view.resource    = resource;
    view.subresource = subresource;

    if (len) {
        view.len = len;
    } else {
        view.len = 1;
        view.len = resource->GetDesc().Width / array_len_in_bytes(view);
    }
    return view;
}

template<typename T>
inline void array_release_resource(MappedView<T>* view) {
    if (view->ptr) array_unmap(view);
    view->resource->Release();
    memset((void*) view, 0, sizeof(*view));
}

// DEPRECATED

struct Descriptor_ {
    LPCWSTR name;
    ID3D12Resource* resource;

    enum Tag {
        NotSet,
        CbvDescriptor,
        SrvDescriptor,
        UavDescriptor
    } tag;
    bool use_default_desc;
    union {
        D3D12_CONSTANT_BUFFER_VIEW_DESC  _cbv_desc; // cbv.BufferLocation will be kept updated by set_descriptor_table_on_heap_
        D3D12_SHADER_RESOURCE_VIEW_DESC  _srv_desc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC _uav_desc;
    };

    inline bool operator !() {
        return this->tag == NotSet;
    }

    static Descriptor_ cbv(ID3D12Resource* resource = NULL, D3D12_CONSTANT_BUFFER_VIEW_DESC  desc = {}, LPCWSTR name = NULL);
    static Descriptor_ srv(ID3D12Resource* resource = NULL, D3D12_SHADER_RESOURCE_VIEW_DESC  desc = {}, LPCWSTR name = NULL);
    static Descriptor_ uav(ID3D12Resource* resource = NULL, D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {}, LPCWSTR name = NULL);

    static Descriptor_ srv_with_default_desc(ID3D12Resource* resource = NULL, LPCWSTR name = NULL);
    static Descriptor_ uav_with_default_desc(ID3D12Resource* resource = NULL, LPCWSTR name = NULL);

    D3D12_CONSTANT_BUFFER_VIEW_DESC&  cbv_desc();
    D3D12_SHADER_RESOURCE_VIEW_DESC&  srv_desc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC& uav_desc();
};

struct DescriptorTable_ {
    UINT offset_into_heap;
    ArrayView<Descriptor_*> descriptors;

    static UINT offset_after(DescriptorTable_* dt);
};

struct RootArgument_ {
    LPCWSTR name;

    enum Tag {
        NotSet,
        RootCbv,
        RootSrv,
        RootUav,
        RootConsts,
        RootDescriptorTable
    } tag;
    union {
        ID3D12Resource** _cbv;
        ID3D12Resource** _srv;
        ID3D12Resource** _uav;
        ArrayView<void> _consts;
        DescriptorTable_* _descriptor_table;
    };

    inline bool operator !() {
        switch (this->tag) {
            case NotSet: {
                return !false;
            } break;
            case RootCbv: {
                return !this->_cbv;
            } break;
            case RootSrv: {
                return !this->_srv;
            } break;
            case RootUav: {
                return !this->_uav;
            } break;
            case RootConsts: {
                return !this->_consts;
            } break;
            case RootDescriptorTable: {
                return !this->_descriptor_table;
            } break;
            default: {
                abort();
            } break;
        }
    }

    static RootArgument_ not_set(LPCWSTR name = NULL);
    static RootArgument_ cbv(ID3D12Resource** cbv = NULL, LPCWSTR name = NULL);
    static RootArgument_ srv(ID3D12Resource** srv = NULL, LPCWSTR name = NULL);
    static RootArgument_ uav(ID3D12Resource** uav = NULL, LPCWSTR name = NULL);
    static RootArgument_ consts(ArrayView<void> consts = {}, LPCWSTR name = NULL);
    static RootArgument_ descriptor_table(DescriptorTable_* descriptor_table = NULL, LPCWSTR name = NULL);
};

void set_descriptor_table_on_heap_(ID3D12DescriptorHeap* descriptor_heap, DescriptorTable_* descriptor_table);
void set_root_arguments_(ID3D12GraphicsCommandList* cmd_list, ID3D12DescriptorHeap* descriptor_heap, ArrayView<RootArgument_> arguments);
