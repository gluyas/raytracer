#include "bluenoise.h"

#include "device.h"
using Device::g_device;

namespace Bluenoise {

#include "out/bluenoise_hlsl.h"

#define DESCRIPTOR_HEAP_SIZE 5

ID3D12CommandAllocator* g_cmd_allocator = NULL;
ID3D12CommandQueue*     g_cmd_queue = NULL;

ID3D12DescriptorHeap* g_descriptor_heap = NULL;

ID3D12RootSignature* g_root_signature = NULL;

// PSOs
ID3D12PipelineState* g_generate_initial_sample_points_pso = NULL;
ID3D12PipelineState* g_sort_initial_sample_points_pso = NULL;
ID3D12PipelineState* g_build_hashtable_pso = NULL;
ID3D12PipelineState* g_generate_sample_points_pso = NULL;

void init() {
    { // g_cmd_allocator, g_cmd_queue
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&g_cmd_allocator));

        D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
        cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        cmd_queue_desc.Type  = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        g_device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&g_cmd_queue));
    }

    { // descriptor heap
        // g_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_descriptor_heap_desc = {};
        g_descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        g_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_descriptor_heap_desc.NumDescriptors = DESCRIPTOR_HEAP_SIZE;
        CHECK_RESULT(g_device->CreateDescriptorHeap(&g_descriptor_heap_desc, IID_PPV_ARGS(&g_descriptor_heap)));
    }

    { // shaders
        // g_root_signature
        CHECK_RESULT(g_device->CreateRootSignature(0, g_generate_sample_points_bytecode, _countof(g_generate_sample_points_bytecode), IID_PPV_ARGS(&g_root_signature)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_root_signature;

        // g_generate_initial_sample_points_pso
        desc.CS = CD3DX12_SHADER_BYTECODE((void*) g_generate_initial_sample_points_bytecode, _countof(g_generate_initial_sample_points_bytecode));
        CHECK_RESULT(g_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_generate_initial_sample_points_pso)));

        // g_sort_initial_sample_points_pso
        desc.CS = CD3DX12_SHADER_BYTECODE((void*) g_sort_initial_sample_points_bytecode, _countof(g_sort_initial_sample_points_bytecode));
        CHECK_RESULT(g_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_sort_initial_sample_points_pso)));

        // g_build_hashtable_pso
        desc.CS = CD3DX12_SHADER_BYTECODE((void*) g_build_hashtable_bytecode, _countof(g_build_hashtable_bytecode));
        CHECK_RESULT(g_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_build_hashtable_pso)));

        // g_generate_sample_points_pso
        desc.CS = CD3DX12_SHADER_BYTECODE((void*) g_generate_sample_points_bytecode, _countof(g_generate_sample_points_bytecode));
        CHECK_RESULT(g_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_generate_sample_points_pso)));
    }
}

UINT generate_sample_points(
    ID3D12Resource** sample_points_buffer,
    ArrayView<Vertex> vertices_array,
    ArrayView<Index>  indices_array,
    float rejection_radius
) {
    // resource management
    Array<ID3D12Resource*> resources = {};      // GPU resources used by kernels
    Array<ID3D12Resource*> temp_resources = {}; // GPU resources used between kernels

    // PREPROCESS MESH
    ComputeGlobals g = {};
    g.rng_seed = rand();

    // accumulate surface area and bounding box
    g.triangles_count = indices_array.len / 3;
    g.total_surface_area = 0;
    Array<float> partial_surface_areas_array = array_init<float>(g.triangles_count);
    Aabb aabb = AABB_NULL;
    for (size_t i = 0; i < g.triangles_count; i += 1) {
        Triangle triangle = triangle_load_from_3_indices(vertices_array, &indices_array[i*3]);

        g.total_surface_area += triangle_area(triangle);
        array_push(&partial_surface_areas_array, g.total_surface_area);

        aabb = aabb_join(aabb, triangle);
    }

    { // define cell grid
        g.rejection_radius = rejection_radius;
        g.grid_cell_width = rejection_radius / sqrt(3);
        XMVECTOR dimensions = XMVectorCeiling((aabb.max - aabb.min) / g.grid_cell_width + XMVectorReplicate(0.5));
        XMVECTOR origin     = aabb.min - 0.5*(g.grid_cell_width*dimensions - (aabb.max - aabb.min));
        XMStoreFloat3(&g.grid_origin, origin);
        XMStoreUInt3(&g.grid_dimensions, dimensions);
    }

    // allocate
    UINT sample_points_upper_bound = (UINT) ceil(g.total_surface_area / (0.5*TAU * 0.25*rejection_radius*rejection_radius));
    UINT initial_sample_points_count = 1;
    while (initial_sample_points_count < 16*sample_points_upper_bound) initial_sample_points_count *= 2;

    // allocate g_hashtable_buffer
    g.hashtable_buckets_count = sample_points_upper_bound;

    // INITIALIZE COMPUTE PIPELINE

    CHECK_RESULT(g_cmd_allocator->Reset());
    ID3D12GraphicsCommandList* cmd_list;
    CHECK_RESULT(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, g_cmd_allocator, NULL, IID_PPV_ARGS(&cmd_list)));

    Fence fence = Fence::make();

    // SRV descriptors
    // data to be copied into SRV buffers is submitted to command lsit
    D3D12_SHADER_RESOURCE_VIEW_DESC base_srv_desc = {}; {
        base_srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        base_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        base_srv_desc.Buffer.FirstElement     = 0;
    }

    Descriptor vertices; {
        ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, vertices_array, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, array_push(&temp_resources));

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = base_srv_desc;
        desc.Format                     = DXGI_FORMAT_UNKNOWN;
        desc.Buffer.NumElements         = vertices_array.len;
        desc.Buffer.StructureByteStride = sizeof(Vertex);
        desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        vertices = Descriptor::make_srv(buffer, desc);
        SET_NAME(vertices.resource);
        array_push(&resources, vertices.resource);
    }

    Descriptor indices; {
        ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, indices_array, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, array_push(&temp_resources));

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = base_srv_desc;
        desc.Format                     = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements         = array_len_in_bytes(&indices_array) / 4;
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_RAW;

        indices = Descriptor::make_srv(buffer, desc);
        SET_NAME(indices.resource);
        array_push(&resources, indices.resource);
    }

    Descriptor partial_surface_areas; {
        ID3D12Resource* buffer = create_buffer_and_write_contents(cmd_list, partial_surface_areas_array, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, array_push(&temp_resources));

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = base_srv_desc;
        desc.Format                     = DXGI_FORMAT_R32_FLOAT;
        desc.Buffer.NumElements         = partial_surface_areas_array.len;
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        partial_surface_areas = Descriptor::make_srv(buffer, desc);
        SET_NAME(partial_surface_areas.resource);
        array_push(&resources, partial_surface_areas.resource);
    }

    // UAV descriptors
    D3D12_UNORDERED_ACCESS_VIEW_DESC base_uav_desc = {}; {
        base_uav_desc.Format              = DXGI_FORMAT_UNKNOWN;
        base_uav_desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        base_uav_desc.Buffer.FirstElement = 0;
    }

    Descriptor initial_sample_points; {
        ID3D12Resource* buffer = create_buffer(initial_sample_points_count*sizeof(InitialSamplePoint), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = base_uav_desc;
        desc.Buffer.StructureByteStride = sizeof(InitialSamplePoint);
        desc.Buffer.NumElements         = initial_sample_points_count;

        initial_sample_points = Descriptor::make_uav(buffer, desc);
        SET_NAME(initial_sample_points.resource);
        array_push(&resources, initial_sample_points.resource);
    }

    Descriptor hashtable; {
        ID3D12Resource* buffer = create_buffer(g.hashtable_buckets_count*sizeof(HashtableBucket), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = base_uav_desc;
        desc.Buffer.StructureByteStride = sizeof(HashtableBucket);
        desc.Buffer.NumElements         = g.hashtable_buckets_count;

        hashtable = Descriptor::make_uav(buffer, desc);
        SET_NAME(hashtable.resource);
        array_push(&resources, hashtable.resource);
    }

    // descriptor table layout
    Descriptor* descriptors[] = {
        &vertices,
        &indices,
        &partial_surface_areas,
        &initial_sample_points,
        &hashtable
    };
    DescriptorTable descriptor_table = { 0, VLA_VIEW(descriptors) };

    set_descriptor_table_on_heap(g_descriptor_heap, &descriptor_table);

    // shader root arguments
    // ComputeGlobals g declared and initialized in mesh preprocessing step
    RootArgument globals = RootArgument::make_cbv(create_buffer(sizeof(ComputeGlobals), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD));
    copy_to_upload_buffer(globals.cbv(), array_of(&g));
    array_push(&resources, globals.cbv());

    ComputeOutput out = {};
    RootArgument output = RootArgument::make_uav(create_buffer(sizeof(ComputeOutput), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT));
    ID3D12Resource* output_readback_buffer = create_buffer(sizeof(ComputeOutput),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    array_push(&resources, output.uav());
    array_push(&resources, output_readback_buffer);

    SortConstants sort = {};
    RootArgument sort_args = RootArgument::make_consts(array_of(&sort));

    SampleGenConstants sample_gen = {};
    RootArgument sample_gen_args = RootArgument::make_consts(array_of(&sample_gen));

    RootArgument descriptor_table_root_argument = RootArgument::make_descriptor_table(&descriptor_table);

    // root argument layout
    RootArgument* root_arguments[] = {
        NULL, // reserve for sort_args and sample_gen_args
        &globals,
        &output,
        &descriptor_table_root_argument,
        NULL // reserve for sample_points
    };

    // DISPATCH INITIALIZATION KERNELS
    cmd_list->SetComputeRootSignature(g_root_signature);
    cmd_list->SetDescriptorHeaps(1, &g_descriptor_heap);
    set_root_arguments(cmd_list, g_descriptor_heap, VLA_VIEW(root_arguments));

    // generate_initial_sample_points
    cmd_list->SetPipelineState(g_generate_initial_sample_points_pso);
    cmd_list->Dispatch(initial_sample_points_count, 1, 1);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(initial_sample_points.resource));

    // sort_initial_sample_points
    cmd_list->SetPipelineState(g_sort_initial_sample_points_pso);
    root_arguments[0] = &sort_args;

    sort.block_size = 1;
    while (sort.block_size < initial_sample_points_count) {
        sort.block_size *= 2;
        sort.pair_offset = sort.block_size;
        do {
            sort.pair_offset /= 2;

            set_root_arguments(cmd_list, g_descriptor_heap, VLA_VIEW(root_arguments));
            cmd_list->Dispatch(initial_sample_points_count/2, 1, 1);
            cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(initial_sample_points.resource));
        } while (sort.pair_offset > 1);
    }
    root_arguments[0] = NULL;

    // build_hashtable
    cmd_list->SetPipelineState(g_build_hashtable_pso);
    cmd_list->Dispatch(initial_sample_points_count, 1, 1);

    // execute initialization
    read_from_buffer(cmd_list, output.uav(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output_readback_buffer);
    CHECK_RESULT(cmd_list->Close());
    g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);

    // sync and get output
    Fence::increment_and_signal_and_wait(g_cmd_queue, &fence);
    copy_from_readback_buffer(array_of(&out), output_readback_buffer);

    Device::release_temp_resources(&temp_resources);
    array_free(&temp_resources);

    // DISPATCH FINAL KERNEL

    // generate random phase group sequence
    UINT phase_sequence[24];
    for (UINT i = 0; i < 24; i++) phase_sequence[i] = i;
    for (UINT i = 0; i < 24; i++) swap(&phase_sequence[rand() % (24-i)], &phase_sequence[23-i]);

    // allocate sample points buffer in out parameter and set root argument
    g.sample_points_capacity = out.hashtable_entry_count;
    copy_to_upload_buffer(globals.cbv(), array_of(&g));

    *sample_points_buffer = create_buffer(g.sample_points_capacity*sizeof(SamplePoint), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RootArgument sample_points = RootArgument::make_uav(*sample_points_buffer);
    root_arguments[4] = &sample_points;

    root_arguments[0] = &sample_gen_args;

    // compare generated sample points between each iteration
    UINT sample_points_count = 0;

    // prepare trials and phase groups for sample point generation
    sample_gen.trial_index = 0;
    while (true) { // generate_sample_points
        CHECK_RESULT(cmd_list->Reset(g_cmd_allocator, g_generate_sample_points_pso));
        cmd_list->SetComputeRootSignature(g_root_signature);
        cmd_list->SetDescriptorHeaps(1, &g_descriptor_heap);

        for (UINT i = 0; i < 24; i += 1) {
            sample_gen.phase_group_index = phase_sequence[i];
            set_root_arguments(cmd_list, g_descriptor_heap, VLA_VIEW(root_arguments));

            cmd_list->Dispatch((g.grid_dimensions.x+2)/3, (g.grid_dimensions.y+2)/3, (g.grid_dimensions.z+2)/3);
            cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(hashtable.resource));
        }
        read_from_buffer(cmd_list, output.uav(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output_readback_buffer);

        CHECK_RESULT(cmd_list->Close());
        g_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList**) &cmd_list);
        Fence::increment_and_signal(g_cmd_queue, &fence);

        if (sample_gen.trial_index >= 1) {
            Fence::wait(g_cmd_queue, &fence, fence.value - 1); // wait for previous iteration while current iteration is executing
            copy_from_readback_buffer(array_of(&out), output_readback_buffer);

            if (out.sample_points_count == sample_points_count) break;
            sample_points_count = out.sample_points_count;
        }
        sample_gen.trial_index += 1;
    }
    Fence::wait(g_cmd_queue, &fence);

    Device::release_temp_resources(&resources);
    array_free(&resources);
    array_free(&partial_surface_areas_array);

    return out.sample_points_count;
}

} // namespace Bluenoise
