#include "raytracing.h"

#include "bluenoise.h"

using Device::g_device;

namespace Raytracing {

#include "out/raytracing_hlsl.h"

ID3D12StateObject*           g_pso        = NULL;
ID3D12StateObjectProperties* g_properties = NULL;

ID3D12RootSignature* g_global_root_signature = NULL;

ShaderIdentifier g_camera_rgen         = {};
ShaderIdentifier g_translucent_rgen    = {};
ShaderIdentifier g_miss                = {};
ShaderIdentifier g_chit[Shader::Count] = {};

ID3D12Resource* g_camera_rgen_shader_record      = NULL;
ID3D12Resource* g_translucent_rgen_shader_record = NULL;
ID3D12Resource* g_hit_group_shader_table = NULL;
ID3D12Resource* g_miss_shader_table      = NULL;

RaytracingGlobals g_globals = {};
ID3D12Resource*   g_globals_buffer = NULL;
ID3D12Resource*   g_globals_upload = NULL;

UINT            g_width, g_height    = 0;
ID3D12Resource* g_render_target      = NULL;
ID3D12Resource* g_sample_accumulator = NULL;

ID3D12Resource* g_scene = NULL;

UINT g_bssrdf_tabulations = 0;
ID3D12Resource* g_bssrdf = NULL;

Array<ShaderRecord> g_shader_table = {};
ID3D12Resource*     g_shader_table_buffer = NULL;
Array<RootArgument> g_root_args = {};

Array<TranslucentProperties> g_translucent_properties           = {};
ID3D12Resource*              g_translucent_properties_buffer    = NULL;
ID3D12Resource*              g_write_translucent_indices_buffer = NULL; // indices into translucent descriptor arrays

DescriptorHandle g_sample_points_descriptor_array = {};

struct TranslucentMesh {
    BluenoisePreprocess preprocess;
    ID3D12Resource* ib; UINT ib_offset;
    ID3D12Resource* vb;
};
struct TranslucentInstance {
    UINT       translucent_id;
    UINT       instance_id;

    XMFLOAT4X4 transform;
    float      scale_factor;

    ID3D12Resource* sample_points_buffer;
    ID3D12Resource* write_sample_points_buffer;
    ID3D12Resource* point_normals_buffer;
    UINT            samples_count;
};
Array<TranslucentMesh>     g_translucent_meshes    = {};
Array<TranslucentInstance> g_translucent_instances = {};
UINT                       g_max_translucent_samples_count = 0;

bool g_enable_translucent_sample_collection = true;
bool g_enable_subsurface_scattering         = true;

void init(ID3D12GraphicsCommandList* cmd_list) {
    { // g_pso, g_properties
        auto pso_desc = CD3DX12_STATE_OBJECT_DESC(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

        auto dxil_subobject = pso_desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>(); {
            auto bytecode = CD3DX12_SHADER_BYTECODE((void *) g_raytracing_hlsl_bytecode, _countof(g_raytracing_hlsl_bytecode));
            dxil_subobject->SetDXILLibrary(&bytecode);
        }

        CHECK_RESULT(g_device->CreateStateObject(pso_desc, IID_PPV_ARGS(&g_pso)));
        CHECK_RESULT(g_pso->QueryInterface(IID_PPV_ARGS(&g_properties)));
    }

    { // g_rgen, g_miss, g_chit
        void* camera_rgen = g_properties->GetShaderIdentifier(L"camera_rgen");
        memcpy(&g_camera_rgen, camera_rgen, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        void* translucent_rgen = g_properties->GetShaderIdentifier(L"translucent_rgen");
        memcpy(&g_translucent_rgen, translucent_rgen, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        void* miss = g_properties->GetShaderIdentifier(L"miss");
        memcpy(&g_miss, miss, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        void* chit[Shader::Count];
        chit[Shader::Lambert]     = g_properties->GetShaderIdentifier(L"lambert_hit_group");
        chit[Shader::Light]       = g_properties->GetShaderIdentifier(L"light_hit_group");
        chit[Shader::Translucent] = g_properties->GetShaderIdentifier(L"translucent_hit_group");
        for (UINT i = 0; i < Shader::Count; i++) {
            memcpy(&g_chit[i], chit[i], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
    };

    // g_global_root_signature
    CHECK_RESULT(g_device->CreateRootSignature(0, g_raytracing_hlsl_bytecode, _countof(g_raytracing_hlsl_bytecode), IID_PPV_ARGS(&g_global_root_signature)));

    g_globals_buffer = create_buffer(sizeof(RaytracingGlobals), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_HEAP_TYPE_DEFAULT);
    g_globals_upload = create_buffer(sizeof(RaytracingGlobals), D3D12_RESOURCE_STATE_GENERIC_READ,                                                                D3D12_HEAP_TYPE_UPLOAD);

    { // g_bssrdf
        #include "data/skin_0.h"
        g_bssrdf_tabulations = round_up(data_len, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        Array<XMFLOAT3> bssrdf = array_init<XMFLOAT3>(g_bssrdf_tabulations);
        for (UINT i = 0; i < data_len; i++) {
            XMFLOAT3 entry;
            entry.x = data_l[i]; // r
            entry.y = data_m[i]; // g
            entry.z = data_s[i]; // b
            array_push(&bssrdf, entry);
        }
        for (UINT i = data_len; i < g_bssrdf_tabulations; i++) array_push_default(&bssrdf);

        // for (UINT i = 0; i < g_bssrdf_tabulations; i++) {
        //     XMVECTOR v = XMVectorReplicate(1.0 - i / (float) (g_bssrdf_tabulations-1));
        //     v *= v; v *= v;
        //     v /= 0.209439510239;
        //     XMStoreFloat3(array_push_uninitialized(&bssrdf), v);
        // }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT bssrdf_footprint = {};
        bssrdf_footprint.Footprint.Format = DXGI_FORMAT_R32G32B32_FLOAT;
        bssrdf_footprint.Footprint.Width  = g_bssrdf_tabulations;
        bssrdf_footprint.Footprint.Height = 1;
        bssrdf_footprint.Footprint.Depth  = 1;
        bssrdf_footprint.Footprint.RowPitch = array_len_in_bytes(&bssrdf);
        g_bssrdf = create_texture_and_write_contents(cmd_list, D3D12_RESOURCE_DIMENSION_TEXTURE1D, &bssrdf_footprint, bssrdf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        SET_NAME(g_bssrdf);
    }
}

void update_resolution(UINT width, UINT height) {
    g_width  = width;
    g_height = height;

    // common render target configuration
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

    { // g_render_target
        if (g_render_target) g_render_target->Release();

        D3D12_RESOURCE_DESC resource_desc = rt_resource_desc;
        resource_desc.Format              = PIXEL_FORMAT;

        CHECK_RESULT(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&g_render_target)
        ));
    }

    { // g_sample_accumulator
        if (g_sample_accumulator) g_sample_accumulator->Release();

        D3D12_RESOURCE_DESC resource_desc = rt_resource_desc;
        resource_desc.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;

        ID3D12Resource* resource;
        CHECK_RESULT(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,
            IID_PPV_ARGS(&g_sample_accumulator)
        ));
    }
}

UINT update_descriptors(DescriptorHandle dest_array) {
    UINT descriptors_count = 0;
    g_root_args.len = 0;

    // g_globals
    array_push(&g_root_args, RootArgument::cbv(g_globals_buffer->GetGPUVirtualAddress()));

    // render target descriptor table
    array_push(&g_root_args, RootArgument::descriptor_table(dest_array + descriptors_count)); {
        // g_render_targets
        g_device->CreateUnorderedAccessView(g_render_target, NULL, NULL, dest_array + descriptors_count);
        descriptors_count += 1;

        // g_sample_accumulator
        g_device->CreateUnorderedAccessView(g_sample_accumulator, NULL, NULL, dest_array + descriptors_count);
        descriptors_count += 1;
    }

    // g_scene
    if (g_scene) array_push(&g_root_args, RootArgument::srv(g_scene->GetGPUVirtualAddress()));
    else         array_push(&g_root_args, {});

    // translucent descriptor table
    array_push(&g_root_args, RootArgument::descriptor_table(dest_array + descriptors_count)); {
        // g_translucent_bssrdf
        g_device->CreateShaderResourceView(g_bssrdf, NULL, dest_array + descriptors_count);
        descriptors_count += 1;

        UINT array_size = g_translucent_meshes.len * g_globals.translucent_instance_stride;

        { // g_translucent_properties
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format                  = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            desc.Buffer.FirstElement        = 0;
            desc.Buffer.NumElements         = array_size;
            desc.Buffer.StructureByteStride = sizeof(*g_translucent_properties.ptr);

            g_device->CreateShaderResourceView(g_translucent_properties_buffer, &desc, dest_array + descriptors_count);
        } descriptors_count += 1;

        {   // g_translucent_samples
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format                  = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            desc.Buffer.FirstElement        = 0;
            desc.Buffer.StructureByteStride = sizeof(SamplePoint);

            // initialize array with null descriptors
            for (UINT i = 0; i < array_size; i++) {
                g_device->CreateShaderResourceView(NULL, &desc, dest_array+descriptors_count + i + 1);
            }

            // populate valid descriptors
            for (auto& instance : g_translucent_instances) {
                UINT index = instance.translucent_id * g_globals.translucent_instance_stride + instance.instance_id;

                desc.Buffer.NumElements = instance.samples_count;
                g_device->CreateShaderResourceView(instance.sample_points_buffer, &desc, dest_array+descriptors_count + index);
            }
            descriptors_count += array_size;
        }
    }

    // second translucent descriptor table
    array_push(&g_root_args, RootArgument::descriptor_table(dest_array + descriptors_count)); {

        { // g_write_translucent_indices
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format                  = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            desc.Buffer.FirstElement        = 0;
            desc.Buffer.NumElements         = g_translucent_instances.len;
            desc.Buffer.StructureByteStride = sizeof(COMMON_UINT);

            g_device->CreateShaderResourceView(g_write_translucent_indices_buffer, &desc, dest_array + descriptors_count);
        }
        descriptors_count += 1;

        // g_write_translucent_samples, g_point_normals
        UINT array_size = g_translucent_meshes.len * 2*g_globals.translucent_instance_stride;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format                  = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension           = D3D12_UAV_DIMENSION_BUFFER;

        uav_desc.Buffer.FirstElement        = 0;
        uav_desc.Buffer.NumElements         = 0;
        uav_desc.Buffer.StructureByteStride = sizeof(SamplePoint);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format                  = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        srv_desc.Buffer.FirstElement        = 0;
        srv_desc.Buffer.NumElements         = 0;
        srv_desc.Buffer.StructureByteStride = sizeof(XMFLOAT3);

        // initialize array with null descriptors
        for (UINT i = 0; i < array_size; i += 2) {
            g_device->CreateUnorderedAccessView(NULL, NULL, &uav_desc, dest_array+descriptors_count + i);
            g_device->CreateShaderResourceView( NULL,       &srv_desc, dest_array+descriptors_count + i + 1);
        }

        // populate valid descriptors
        for (auto& instance : g_translucent_instances) {
            // increased stride to fit point_normals descriptors
            UINT index = instance.translucent_id * 2*g_globals.translucent_instance_stride + 2*instance.instance_id;

            uav_desc.Buffer.NumElements = instance.samples_count;
            srv_desc.Buffer.NumElements = instance.samples_count;
            g_device->CreateUnorderedAccessView(instance.write_sample_points_buffer, NULL, &uav_desc, dest_array+descriptors_count + index);
            g_device->CreateShaderResourceView( instance.point_normals_buffer,             &srv_desc, dest_array+descriptors_count + index + 1);
        }
        descriptors_count += array_size;
    }

    return descriptors_count;
}

Blas build_blas(
    ID3D12GraphicsCommandList4* cmd_list,
    ArrayView<GeometryInstance> geometries,
    Array<ID3D12Resource*>* temp_resources
) {
    Blas blas = {};
    blas.shader_table_index    = g_shader_table.len;
    blas.translucent_ids_index = g_translucent_properties.len;
    blas.translucent_ids_count = 0;

    Array<Vertex> vertices = {};
    Array<Index>  indices  = {};

    Array<D3D12_RAYTRACING_GEOMETRY_DESC> geometry_descs = array_init<D3D12_RAYTRACING_GEOMETRY_DESC>(geometries.len);

    for (auto& geometry : geometries) {
        // create shader record for material shader
        ShaderRecord shader_record = {}; {
            shader_record.ident = g_chit[geometry.material.shader];

            shader_record.locals.color          = geometry.material.color;
            shader_record.locals.translucent_id = -1; // generate translucent properties after mesh upload

            // later increment by gpu virtual addresses of vb and ib
            shader_record.vertices = 0;
            shader_record.indices  = array_len_in_bytes(&indices);
        };
        array_push(&g_shader_table, shader_record);

        // create geometry desc for blas build with relative GPU virtual addresses and unknown vertex count
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {}; {
            desc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

            desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            desc.Triangles.VertexCount  = 0;
            desc.Triangles.VertexBuffer = { 0, sizeof(Vertex) };

            desc.Triangles.IndexFormat  = INDEX_FORMAT;
            desc.Triangles.IndexCount   = geometry.indices.len;
            desc.Triangles.IndexBuffer  = array_len_in_bytes(&indices);
        };
        array_push(&geometry_descs, desc);

        // concat mesh data, taking into account combined mesh offset
        array_reserve(&indices, geometry.indices.len);
        for (auto& index : geometry.indices) {
            array_push(&indices, (Index)  (vertices.len + index));
        }
        array_concat(&vertices, &geometry.vertices);
    }
    if (vertices.len > INDEX_MAX) abort();

    // upload vb and ib to gpu and get virtual addresses
    blas.vb = create_buffer_and_write_contents(cmd_list, vertices, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Device::push_uninitialized_temp_resource(temp_resources));
    SET_NAME(blas.vb);
    blas.ib = create_buffer_and_write_contents(cmd_list, indices,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Device::push_uninitialized_temp_resource(temp_resources));
    SET_NAME(blas.ib);

    // final geometry pass
    ArrayView<ShaderRecord> shader_records = array_slice_from(&g_shader_table, blas.shader_table_index);
    for (UINT i = 0; i < geometries.len; i++) {
        // prepare translucent geometry
        if (geometries[i].material.shader == Shader::Translucent) {
            blas.translucent_ids_count += 1;

            shader_records[i].locals.translucent_id = g_translucent_properties.len;

            TranslucentMesh mesh = {};
            mesh.vb        = blas.vb;
            mesh.ib        = blas.ib;
            mesh.ib_offset = shader_records[i].indices / sizeof(Index);

            ArrayView<Index> ib = array_slice(&indices, mesh.ib_offset, mesh.ib_offset + geometries[i].indices.len);
            mesh.preprocess = Bluenoise::preprocess_mesh_data(cmd_list, vertices, ib, temp_resources);

            // TODO: translucent properties from material
            TranslucentProperties properties = {};
            // calculate sample point area after sample point generation

            array_push(&g_translucent_meshes,     mesh);
            array_push(&g_translucent_properties, properties);
        }

        // update shader table
        shader_records[i].vertices += blas.vb->GetGPUVirtualAddress();
        shader_records[i].indices  += blas.ib->GetGPUVirtualAddress();

        geometry_descs[i].Triangles.VertexBuffer.StartAddress += blas.vb->GetGPUVirtualAddress();
        geometry_descs[i].Triangles.IndexBuffer               += blas.ib->GetGPUVirtualAddress();

        geometry_descs[i].Triangles.VertexCount = vertices.len;
    }

    // build acceleration structure
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {}; {
        inputs.Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        inputs.NumDescs       = geometry_descs.len;
        inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pGeometryDescs = geometry_descs.ptr;
    };

    // get prebuild info and allocate resources
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
    g_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    blas.blas               = create_buffer(prebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ID3D12Resource* scratch = create_buffer(prebuild.ScratchDataSizeInBytes,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Device::push_temp_resource(scratch, temp_resources);

    // execute build on command list
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {}; {
        build.Inputs = inputs;

        build.DestAccelerationStructureData    = blas.blas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    };
    cmd_list->BuildRaytracingAccelerationStructure(&build, 0, NULL);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(blas.blas));

    // cleanup
    array_free(&vertices);
    array_free(&indices);
    array_free(&geometry_descs);

    return blas;
}

void build_tlas(
    ID3D12GraphicsCommandList4* cmd_list,
    ArrayView<BlasInstance> instances,
    Array<ID3D12Resource*>* temp_resources
) {
    // reset translucent mesh instances
    for (auto& instance : g_translucent_instances) {
        if (!instance.samples_count) continue;

        instance.sample_points_buffer->Release();
        instance.write_sample_points_buffer->Release();
        instance.point_normals_buffer->Release();
    }
    g_translucent_instances.len           = 0;
    g_max_translucent_samples_count       = 0;
    g_globals.translucent_instance_stride = 0;

    Array<Pair<UINT, UINT>> blas_instance_counts      = {}; // blas->shader_table_index -> blas instance counts

    // instance descs for tlas build
    MappedView<D3D12_RAYTRACING_INSTANCE_DESC> descs = array_init_mapped_upload_buffer<D3D12_RAYTRACING_INSTANCE_DESC>(instances.len);

    for (int i = 0; i < instances.len; i++) {
        BlasInstance* instance = &instances[i];

        // update instance counts
        UINT* count = array_get_by_key(&blas_instance_counts, instance->blas->shader_table_index);
        if (count) {
            *count += 1;
        } else {
            ptrdiff_t index = array_insert_by_key(&blas_instance_counts, { instance->blas->shader_table_index, 1 });
            count = &blas_instance_counts[index]._1;
        }
        UINT instance_id = *count - 1;

        if (instance->blas->translucent_ids_count > 0) {
            // instantiate translucent mesh
            for (int j = 0; j < instance->blas->translucent_ids_count; j++) {
                TranslucentInstance* translucent = array_push_default(&g_translucent_instances);
                translucent->translucent_id = instance->blas->translucent_ids_index + j;
                translucent->instance_id    = instance_id; // NOTE: assumes translucent instantiations = blas instantiations => no overlap in blas translucent ranges

                translucent->transform      = instance->transform;
            }
            // update stride to fit duplicated translucent meshes
            g_globals.translucent_instance_stride = max(g_globals.translucent_instance_stride, *count);
        }

        // create ratracing instance desc
        D3D12_RAYTRACING_INSTANCE_DESC desc = {}; {
            desc.InstanceMask = 1;
            desc.Flags        = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

            XMStoreFloat3x4((XMFLOAT3X4*) &desc.Transform, XMLoadFloat4x4(&instance->transform));

            desc.InstanceID                          = instance_id;
            desc.InstanceContributionToHitGroupIndex = instance->blas->shader_table_index;
            desc.AccelerationStructure               = instance->blas->blas->GetGPUVirtualAddress();
        };
        descs[i] = desc;
    }
    array_free(&blas_instance_counts);
    array_unmap_resource(&descs);
    Device::push_temp_resource(descs.resource, temp_resources);

    // upload shader table
    if (g_shader_table_buffer) g_shader_table_buffer->Release();
    if (g_shader_table.len > 0) {
        g_camera_rgen_shader_record      = create_buffer_and_write_contents(cmd_list, array_of(&Raytracing::g_camera_rgen),      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        g_translucent_rgen_shader_record = create_buffer_and_write_contents(cmd_list, array_of(&Raytracing::g_translucent_rgen), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        g_miss_shader_table              = create_buffer_and_write_contents(cmd_list, array_of(&Raytracing::g_miss),             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);
        g_hit_group_shader_table         = create_buffer_and_write_contents(cmd_list, g_shader_table,                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL);

        g_shader_table_buffer = create_buffer_and_write_contents(cmd_list, g_shader_table, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Device::push_uninitialized_temp_resource(temp_resources));
    }

    // build acceleration strucutre
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {}; {
        inputs.Type  = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        inputs.NumDescs      = instances.len;
        inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.InstanceDescs = descs.resource->GetGPUVirtualAddress();
    };

    // get prebuild info and allocate resources
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild;
    g_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    ID3D12Resource* tlas    = create_buffer(prebuild.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ID3D12Resource* scratch = create_buffer(prebuild.ScratchDataSizeInBytes,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Device::push_temp_resource(scratch, temp_resources);

    // execute build on command list
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {}; {
        build.Inputs = inputs;

        build.DestAccelerationStructureData    = tlas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    };
    cmd_list->BuildRaytracingAccelerationStructure(&build, 0, NULL);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(tlas));

    g_scene = tlas;
}

UINT generate_translucent_samples(ID3D12GraphicsCommandList4* cmd_list, float radius, Array<ID3D12Resource*>* temp_resources) {
    if (g_globals.translucent_instance_stride == 0) return 0;

    UINT total_sample_points = 0;
    g_max_translucent_samples_count = 0;

    // upload translucent properties in instanced array layout
    if (g_translucent_properties_buffer) g_translucent_properties_buffer->Release();
    MappedView<TranslucentProperties> properties_upload = array_init_mapped_upload_buffer<TranslucentProperties>(g_translucent_properties.len * g_globals.translucent_instance_stride);

    // log indices of translucent instances for sample collection
    MappedView<UINT> write_translucent_indices = array_init_mapped_upload_buffer<UINT>(g_translucent_instances.len);

    // batch clone sample points buffer for write target
    Array<D3D12_RESOURCE_BARRIER>                            pre_copy  = array_init<D3D12_RESOURCE_BARRIER>(g_translucent_instances.len);
    Array<D3D12_RESOURCE_BARRIER>                            post_copy = array_init<D3D12_RESOURCE_BARRIER>(2*g_translucent_instances.len);
    Array<Triplet<ID3D12Resource*, ID3D12Resource*, UINT64>> copies    = array_init<Triplet<ID3D12Resource*, ID3D12Resource*, UINT64>>(g_translucent_instances.len);

    for (UINT i = 0; i < g_translucent_instances.len; i++) {
        TranslucentInstance*   instance   = &g_translucent_instances[i];

        TranslucentMesh*       mesh       = &g_translucent_meshes[instance->translucent_id];
        TranslucentProperties* properties = &g_translucent_properties[instance->translucent_id];

        if (instance->samples_count) {
            // release previous instances' resources
            instance->sample_points_buffer->Release();
            instance->point_normals_buffer->Release();
            instance->write_sample_points_buffer->Release();
        }

        instance->samples_count = Bluenoise::generate_sample_points(
            &instance->sample_points_buffer,
            &instance->point_normals_buffer,
            &instance->scale_factor,

            &mesh->preprocess,
            mesh->ib, mesh->ib_offset,
            mesh->vb,
            &instance->transform,
            radius
        );
        properties->samples_mean_area = instance->scale_factor*instance->scale_factor * mesh->preprocess.total_surface_area / instance->samples_count;
        total_sample_points += instance->samples_count;

        // insert instance properties into upload buffer
        UINT index = instance->translucent_id * g_globals.translucent_instance_stride + instance->instance_id;
        properties_upload[index] = *properties;

        // update translucent_rgen dispatch dimensions
        g_max_translucent_samples_count = max(instance->samples_count, g_max_translucent_samples_count);
        write_translucent_indices[i] = index;

        // prepare to clone sample points and create destination buffer
        UINT64 size = instance->samples_count*sizeof(SamplePoint);
        instance->write_sample_points_buffer = create_buffer(size, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        instance->write_sample_points_buffer->SetName(L"write_sample_points");

        *array_push_uninitialized(&pre_copy)  = CD3DX12_RESOURCE_BARRIER::Transition(instance->sample_points_buffer,       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        *array_push_uninitialized(&post_copy) = CD3DX12_RESOURCE_BARRIER::Transition(instance->sample_points_buffer,       D3D12_RESOURCE_STATE_COPY_SOURCE,      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        *array_push_uninitialized(&post_copy) = CD3DX12_RESOURCE_BARRIER::Transition(instance->write_sample_points_buffer, D3D12_RESOURCE_STATE_COPY_DEST,        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        array_push(&copies, { instance->write_sample_points_buffer, instance->sample_points_buffer, size });
    }

    // perform sample points clone
    cmd_list->ResourceBarrier(pre_copy.len,  pre_copy.ptr);
    for (const auto& copy : copies) cmd_list->CopyBufferRegion(copy._0, 0, copy._1, 0, copy._2);
    cmd_list->ResourceBarrier(post_copy.len, post_copy.ptr);

    array_free(&pre_copy); array_free(&post_copy); array_free(&copies);

    // copy translucent properties from upload buffer to main buffer
    g_translucent_properties_buffer = create_buffer(array_len_in_bytes(&properties_upload), D3D12_RESOURCE_STATE_COPY_DEST);
    cmd_list->CopyResource(g_translucent_properties_buffer, properties_upload.resource);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_translucent_properties_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    array_unmap_resource(&properties_upload);
    Device::push_temp_resource(properties_upload.resource, temp_resources);

    // copy translucent indices
    if (g_write_translucent_indices_buffer && g_write_translucent_indices_buffer->GetDesc().Width < array_len_in_bytes(&write_translucent_indices)) {
        g_write_translucent_indices_buffer->Release();
        g_write_translucent_indices_buffer = NULL;
    }
    if (g_write_translucent_indices_buffer) {
        cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_write_translucent_indices_buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    } else {
        g_write_translucent_indices_buffer = create_buffer(array_len_in_bytes(&write_translucent_indices), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    cmd_list->CopyBufferRegion(g_write_translucent_indices_buffer, 0, write_translucent_indices.resource, 0, array_len_in_bytes(&write_translucent_indices));
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_write_translucent_indices_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    array_unmap_resource(&write_translucent_indices);
    Device::push_temp_resource(write_translucent_indices.resource, temp_resources);

    return total_sample_points;
}

void dispatch_rays(ID3D12GraphicsCommandList4* cmd_list) {
    float translucent_bssrdf_fudge = g_globals.translucent_bssrdf_fudge;
    if (!g_enable_subsurface_scattering) {
        g_globals.translucent_bssrdf_fudge = 0;
    }

    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_globals_buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    copy_to_upload_buffer(g_globals_upload, array_of(&g_globals));
    cmd_list->CopyResource(g_globals_buffer, g_globals_upload);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_globals_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    cmd_list->SetComputeRootSignature(Raytracing::g_global_root_signature);
    RootArgument::set_on_command_list(cmd_list, g_root_args);
    cmd_list->SetPipelineState1(Raytracing::g_pso);

    D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
    dispatch_rays.HitGroupTable.StartAddress  = g_hit_group_shader_table->GetGPUVirtualAddress();
    dispatch_rays.HitGroupTable.SizeInBytes   = g_hit_group_shader_table->GetDesc().Width;
    dispatch_rays.HitGroupTable.StrideInBytes = sizeof(ShaderRecord);

    dispatch_rays.MissShaderTable.StartAddress  = g_miss_shader_table->GetGPUVirtualAddress();
    dispatch_rays.MissShaderTable.SizeInBytes   = g_miss_shader_table->GetDesc().Width;
    dispatch_rays.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

    if (g_enable_translucent_sample_collection) {
        // dispatch translucent samples
        dispatch_rays.RayGenerationShaderRecord.StartAddress = g_translucent_rgen_shader_record->GetGPUVirtualAddress();
        dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = g_translucent_rgen_shader_record->GetDesc().Width;

        dispatch_rays.Width  = g_max_translucent_samples_count;
        dispatch_rays.Height = g_translucent_instances.len;
        dispatch_rays.Depth  = 1;

        cmd_list->DispatchRays(&dispatch_rays);
    }

    // dispatch render
    dispatch_rays.RayGenerationShaderRecord.StartAddress = g_camera_rgen_shader_record->GetGPUVirtualAddress();
    dispatch_rays.RayGenerationShaderRecord.SizeInBytes  = g_camera_rgen_shader_record->GetDesc().Width;

    dispatch_rays.Width  = g_width;
    dispatch_rays.Height = g_height;
    dispatch_rays.Depth  = 1;

    cmd_list->DispatchRays(&dispatch_rays);

    // copy translucent buffers
    static Array<D3D12_RESOURCE_BARRIER> pre_copy_barriers = {};
    static Array<D3D12_RESOURCE_BARRIER> post_copy_barriers = {};
    pre_copy_barriers.len = 0;
    post_copy_barriers.len = 0;

    if (g_enable_translucent_sample_collection) {
        for (auto& instance : g_translucent_instances) {
            *array_push_uninitialized(&pre_copy_barriers)  = CD3DX12_RESOURCE_BARRIER::UAV(instance.write_sample_points_buffer);

            *array_push_uninitialized(&pre_copy_barriers)  = CD3DX12_RESOURCE_BARRIER::Transition(instance.write_sample_points_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,          D3D12_RESOURCE_STATE_COPY_SOURCE);
            *array_push_uninitialized(&pre_copy_barriers)  = CD3DX12_RESOURCE_BARRIER::Transition(instance.sample_points_buffer,       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            *array_push_uninitialized(&post_copy_barriers) = CD3DX12_RESOURCE_BARRIER::Transition(instance.write_sample_points_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            *array_push_uninitialized(&post_copy_barriers) = CD3DX12_RESOURCE_BARRIER::Transition(instance.sample_points_buffer,       D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    *array_push_uninitialized(&post_copy_barriers) = CD3DX12_RESOURCE_BARRIER::UAV(g_render_target);
    *array_push_uninitialized(&post_copy_barriers) = CD3DX12_RESOURCE_BARRIER::UAV(g_sample_accumulator);

    if (pre_copy_barriers.len)  cmd_list->ResourceBarrier(pre_copy_barriers.len,  pre_copy_barriers.ptr);
    if (g_enable_translucent_sample_collection) {
        for (auto& instance : g_translucent_instances) {
            cmd_list->CopyBufferRegion(instance.sample_points_buffer, 0, instance.write_sample_points_buffer, 0, instance.samples_count*sizeof(SamplePoint));
        }
    }
    if (post_copy_barriers.len) cmd_list->ResourceBarrier(post_copy_barriers.len, post_copy_barriers.ptr);

    g_globals.accumulator_count             += 1;
    g_globals.translucent_accumulator_count += g_enable_translucent_sample_collection;
    g_globals.translucent_bssrdf_fudge = translucent_bssrdf_fudge;
}

} // namespace Raytracing
