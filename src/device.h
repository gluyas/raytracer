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

struct Descriptor {
    ID3D12Resource* resource;

    enum Tag {
        CbvDescriptor,
        SrvDescriptor,
        UavDescriptor
    } tag;
    union {
        D3D12_CONSTANT_BUFFER_VIEW_DESC  _cbv_desc; // cbv.BufferLocation must be updated with resource's address on CBV creation
        D3D12_SHADER_RESOURCE_VIEW_DESC  _srv_desc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC _uav_desc;
    };

    static Descriptor make_cbv(ID3D12Resource* resource = NULL, D3D12_CONSTANT_BUFFER_VIEW_DESC  desc = {});
    static Descriptor make_srv(ID3D12Resource* resource = NULL, D3D12_SHADER_RESOURCE_VIEW_DESC  desc = {});
    static Descriptor make_uav(ID3D12Resource* resource = NULL, D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {});

    D3D12_CONSTANT_BUFFER_VIEW_DESC&  cbv_desc();
    D3D12_SHADER_RESOURCE_VIEW_DESC&  srv_desc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC& uav_desc();
};

struct DescriptorTable {
    UINT offset_into_heap;
    ArrayView<Descriptor*> descriptors;
};

struct RootArgument {
    enum Tag {
        RootCbv,
        RootSrv,
        RootUav,
        RootConsts,
        RootDescriptorTable
    } tag;
    union {
        ID3D12Resource* _cbv;
        ID3D12Resource* _srv;
        ID3D12Resource* _uav;
        ArrayView<void> _consts;
        DescriptorTable* _descriptor_table;
    };

    static RootArgument make_cbv(ID3D12Resource* cbv = NULL);
    static RootArgument make_srv(ID3D12Resource* srv = NULL);
    static RootArgument make_uav(ID3D12Resource* uav = NULL);
    static RootArgument make_consts(ArrayView<void> consts = {});
    static RootArgument make_descriptor_table(DescriptorTable* descriptor_table = NULL);

    ID3D12Resource*& cbv();
    ID3D12Resource*& srv();
    ID3D12Resource*& uav();
    ArrayView<void>& consts();
    DescriptorTable*& descriptor_table();
};

namespace Device {

extern ID3D12Device5* g_device;
extern IDXGIFactory7* g_dxgi_factory;
#ifdef DEBUG
extern ID3D12Debug* g_debug;
#endif // DEBUG

extern ID3D12CommandAllocator* g_debug_cmd_allocator;
extern ID3D12CommandQueue* g_debug_cmd_queue;
extern ID3D12CommandAllocator* g_debug_cmd_allocator;

extern UINT g_descriptor_size;
extern UINT g_rtv_descriptor_size;

void init();
void release_temp_resources(Array<ID3D12Resource*>* temp_resources = NULL);

} // namespace Device

ID3D12Resource* create_buffer(UINT64 size_in_bytes, D3D12_RESOURCE_STATES initial_state, D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
ID3D12Resource* create_buffer_and_write_contents(ID3D12GraphicsCommandList* cmd_list, ArrayView<void> data, D3D12_RESOURCE_STATES initial_state, ID3D12Resource** upload_buffer, D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

void copy_to_upload_buffer(ID3D12Resource* dst, ArrayView<void> src);
void copy_from_readback_buffer(ArrayView<void> dst, ID3D12Resource* src);
UINT64 copy_from_readback_buffer(Array<void>* dst, ID3D12Resource* src);

void write_to_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ArrayView<void> src, ID3D12Resource** upload_buffer);
ID3D12Resource* read_from_buffer(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, ID3D12Resource** readback_buffer);

void set_descriptor_table_on_heap(ID3D12DescriptorHeap* descriptor_heap, DescriptorTable* descriptor_table);
void set_root_arguments(ID3D12GraphicsCommandList* cmd_list, ID3D12DescriptorHeap* descriptor_heap, ArrayView<RootArgument*> arguments);
