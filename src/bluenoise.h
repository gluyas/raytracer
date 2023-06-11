#pragma once
#include "prelude.h"

#ifdef CPP

struct BluenoisePreprocess {
    UINT indices_count;

    Aabb            aabb;
    float           total_surface_area;
    ID3D12Resource* partial_surface_areas;
};

namespace Bluenoise {
#endif // CPP
struct ComputeGlobals {
    COMMON_UINT     rng_seed;
    COMMON_UINT     triangles_count;
    COMMON_UINT     indices_offset;
    COMMON_FLOAT    total_surface_area;

    COMMON_FLOAT4X4 transform;
    COMMON_FLOAT    rejection_radius;

    COMMON_FLOAT3   grid_origin;
    COMMON_FLOAT    grid_cell_width;
    COMMON_UINT3    grid_dimensions;

    COMMON_UINT     hashtable_buckets_count;

    COMMON_UINT     sample_points_capacity;
};

struct SortConstants {
    COMMON_UINT block_size;
    COMMON_UINT pair_offset;
};

struct SampleGenConstants {
    COMMON_UINT phase_group_index;
    COMMON_UINT trial_index;
};

struct ComputeOutput {
    COMMON_UINT hashtable_overflow_count;
    COMMON_UINT hashtable_entry_count;
    COMMON_UINT sample_points_count;
};

struct InitialSamplePoint {
    COMMON_FLOAT3 position;
    COMMON_UINT   triangle_id;
};

struct HashtableEntry {
    COMMON_UINT   cell_id;                     // key
    COMMON_UINT   initial_sample_point_index;  // index into g_initial_sample_points
    COMMON_FLOAT3 selected_sample_position;    // only pick up to 1 point in each cell: initialize with infinity
};

#define BLUENOISE_HASHTABLE_BUCKET_SIZE 5 // TODO: make into root argument?
struct HashtableBucket {
    COMMON_UINT    entries_count;
    HashtableEntry entries[BLUENOISE_HASHTABLE_BUCKET_SIZE];
};

#ifdef CPP

void init();

BluenoisePreprocess preprocess_mesh_data(
    ID3D12GraphicsCommandList* cmd_list,
    ArrayView<Vertex> vertices,
    ArrayView<Index>  indices,
    Array<ID3D12Resource*>* temp_resources
);

UINT generate_sample_points(
    ID3D12Resource** sample_points_buffer,
    ID3D12Resource** point_normals_buffer,
    float* scale_factor,

    BluenoisePreprocess* preprocess,
    ID3D12Resource* ib, UINT ib_offset,
    ID3D12Resource* vb,
    ID3D12Resource* tex,
    XMFLOAT4X4* transform,
    float rejection_radius
);

} // namespace Bluenoise
#endif // CPP
