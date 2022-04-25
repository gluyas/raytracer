#pragma once
#include "prelude.h"

#include "device.h"

__declspec(align(32)) struct ShaderIdentifier {
    unsigned char bytes[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
};

// shader record for chit shaders
__declspec(align(32)) struct ShaderRecord {
    ShaderIdentifier ident;

    RaytracingLocals            locals;
    D3D12_GPU_VIRTUAL_ADDRESS   vertices;
    D3D12_GPU_VIRTUAL_ADDRESS   indices;
};

enum Shader {
    Lambert = 0,
    Light,
    Translucent,

    Count,
};

struct Material {
    Shader   shader;
    XMFLOAT3 color;
};

struct GeometryInstance {
    ArrayView<Vertex> vertices;
    ArrayView<Index>  indices;
    Material          material;
};

struct Blas {
    ID3D12Resource* blas;
    ID3D12Resource* vb;
    ID3D12Resource* ib;

    UINT shader_table_index;
    UINT translucent_ids_index, translucent_ids_count; // used to duplicate sample points if necessary
};

struct BlasInstance {
    XMFLOAT4X4 transform;
    Blas* blas;
};

namespace Raytracing {

extern ID3D12StateObject*           g_pso;
extern ID3D12StateObjectProperties* g_properties;

extern ID3D12RootSignature* g_global_root_signature;

extern ID3D12Resource* g_render_target;

extern RaytracingGlobals g_globals;

extern bool g_enable_translucent_sample_collection;
extern bool g_enable_subsurface_scattering;

void init(ID3D12GraphicsCommandList* cmd_list);

void update_resolution(UINT width, UINT height);
UINT update_descriptors(DescriptorHandle dest_array);

Blas build_blas(
    ID3D12GraphicsCommandList4* cmd_list,
    ArrayView<GeometryInstance> geometries,
    Array<ID3D12Resource*>* temp_resources = NULL
);

void build_tlas(
    ID3D12GraphicsCommandList4* cmd_list,
    ArrayView<BlasInstance> instances,
    Array<ID3D12Resource*>* temp_resources = NULL
);

UINT generate_translucent_samples(ID3D12GraphicsCommandList4* cmd_list, float radius, Array<ID3D12Resource*>* temp_resources = NULL);

void dispatch_rays(ID3D12GraphicsCommandList4* cmd_list);

}
