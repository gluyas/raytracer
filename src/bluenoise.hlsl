#include "bluenoise.h"

#include "random.hlsl"

// root signature:
// 0: g_sort | g_sample_gen
// 1: g
// 2: g_out
// 3: {
//     g_vertices, g_indices, g_partial_surface_areas,
//     g_initial_sample_points, g_hashtable
// }
// 4: g_sample_points
// 5: g_point_normals
#define ROOT_SIG "RootFlags(0)," \
    "RootConstants(num32BitConstants=2, b0)," \
    "CBV(b1)," \
    "UAV(u0)," \
    "DescriptorTable(" \
        "SRV(t0, numDescriptors = 3)," \
        "UAV(u1, numDescriptors = 2)" \
    ")," \
    "UAV(u3)," \
    "UAV(u4)"

ConstantBuffer<SortConstants>      g_sort       : register(b0);
ConstantBuffer<SampleGenConstants> g_sample_gen : register(b0);

ConstantBuffer<ComputeGlobals>    g     : register(b1);
RWStructuredBuffer<ComputeOutput> g_out : register(u0);

StructuredBuffer<Vertex> g_vertices : register(t0);
ByteAddressBuffer        g_indices  : register(t1);

Buffer<float> g_partial_surface_areas : register(t2);

RWStructuredBuffer<InitialSamplePoint> g_initial_sample_points : register(u1);

RWStructuredBuffer<HashtableBucket> g_hashtable : register(u2);

RWStructuredBuffer<SamplePoint> g_sample_points : register(u3);
RWStructuredBuffer<float3>      g_point_normals : register(u4);

uint hash_from_cell_id(uint cell_id) {
    return cell_id % g.hashtable_buckets_count;
}

uint3 cell_from_position(float3 position) {
    return uint3(floor((position - g.grid_origin) / g.grid_cell_width));
}

uint cell_id_from_cell(uint3 cell) {
    return cell.x + cell.y*g.grid_dimensions.x + cell.z*g.grid_dimensions.x*g.grid_dimensions.y;
}

uint cell_id_from_position(float3 position) {
    return cell_id_from_cell(cell_from_position(position));
}

[numthreads(1, 1, 1)]
[RootSignature(ROOT_SIG)]
void generate_initial_sample_points(uint3 index : SV_DispatchThreadID) {
    InitialSamplePoint sample_point;

    uint rng = hash(uint2(index.x, g.rng_seed));

    // pick random triangle on mesh weighted by surface area
    float x = random01(rng) * g.total_surface_area;

    // binary search
    uint min_index = 0;
    uint max_index = g.triangles_count - 1;
    while (min_index != max_index) {
        uint i = (min_index + max_index) / 2;
        if (x > g_partial_surface_areas[i]) {
            min_index = i + 1;
        } else {
            max_index = i;
        }
    }
    sample_point.triangle_id = min_index;

    // pick uniform random point on triangle surface
    Vertex vertices[3] = load_triangle_vertices(g_vertices, load_3x16bit_indices(g_indices, g.indices_offset/3 + sample_point.triangle_id));
    float3 p0 = vertices[0].position;
    float3 p1 = vertices[1].position;
    float3 p2 = vertices[2].position;

    // generate random barycentrics
    float2 u = float2(random01(rng), random01(rng));
    u.x = sqrt(u.x);
    u.y = u.y * u.x;
    u.x = 1 - u.x;
    sample_point.position = p0 + u.x*(p1-p0) + u.y*(p2-p0);

    g_initial_sample_points[index.x] = sample_point;

    if (all(index == 0)) {
        // g_out is not used in this kernel, so reset it for use with atomics in later kernels
        ComputeOutput reinit = { 0, 0, 0 };
        g_out[0] = reinit;
    }
}

[numthreads(1, 1, 1)]
[RootSignature(ROOT_SIG)]
void sort_initial_sample_points(uint3 index : SV_DispatchThreadID) {
    uint offset     = g_sort.pair_offset;
    uint block_size = g_sort.block_size;

    // determine indices to compare and direction of comparison
    // the lower of the two will be stored in min_index;
    uint min_index = ((index.x / offset) * 2*offset) + (index.x % offset);
    uint max_index = min_index + offset;
    bool reverse   = (min_index / g_sort.block_size) % 2;
    if (reverse) {
        uint swap = min_index;
        min_index = max_index;
        max_index = swap;
    }

    // sort pair
    InitialSamplePoint min_point = g_initial_sample_points[min_index];
    InitialSamplePoint max_point = g_initial_sample_points[max_index];
    if (cell_id_from_position(min_point.position) > cell_id_from_position(max_point.position)) {
        g_initial_sample_points[min_index] = max_point;
        g_initial_sample_points[max_index] = min_point;
    }
}

// determine cell grid bounds and resolution on host

[numthreads(1, 1, 1)]
[RootSignature(ROOT_SIG)]
void build_hashtable(uint3 index : SV_DispatchThreadID) {
    uint cell_id = cell_id_from_position(g_initial_sample_points[index.x].position);

    if (index.x > 0) {
        uint prev_cell_id = cell_id_from_position(g_initial_sample_points[index.x - 1].position);
        if (prev_cell_id == cell_id) return;
    }
    // this is the first point of the cell: create hashtable entry
    HashtableEntry entry;
    entry.cell_id                    = cell_id;
    entry.initial_sample_point_index = index.x;
    entry.selected_sample_position   = INFINITY;

    uint hash = hash_from_cell_id(cell_id);
    uint entry_index;
    InterlockedAdd(g_hashtable[hash].entries_count, 1, entry_index);
    if (entry_index >= BLUENOISE_HASHTABLE_BUCKET_SIZE) {
        // overflow situation
        InterlockedAdd(g_out[0].hashtable_overflow_count, 1);
        return;
    }
    InterlockedAdd(g_out[0].hashtable_entry_count, 1);
    g_hashtable[hash].entries[entry_index] = entry;
}

inline bool hashtable_search(in uint cell_id, out HashtableEntry entry, out uint hash, out uint bucket_index) {
    hash = hash_from_cell_id(cell_id);
    HashtableBucket bucket = g_hashtable[hash];
    for (bucket_index = 0; bucket_index < bucket.entries_count; bucket_index += 1) {
        entry = bucket.entries[bucket_index];
        if (entry.cell_id == cell_id) {
            return true;
        }
    }
    return false;
}

inline bool hashtable_search(in uint cell_id, out HashtableEntry entry) {
    uint _hash, _bucket_index;
    return hashtable_search(cell_id, entry, _hash, _bucket_index);
}

// determine phase group order on host
// iteratively call this kernel until enough points have been generated

[numthreads(1, 1, 1)]
[RootSignature(ROOT_SIG)]
void generate_sample_points(uint3 id : SV_DispatchThreadID) {
    if (g_out[0].sample_points_count >= g.sample_points_capacity) return;

    // determine grid cell for this kernel instance
    uint3 phase_group;
    phase_group.x = (g_sample_gen.phase_group_index)     % 3;
    phase_group.y = (g_sample_gen.phase_group_index / 3) % 3;
    phase_group.z = (g_sample_gen.phase_group_index / 9);

    uint3 cell = 3*id + phase_group;
    if (any(cell >= g.grid_dimensions)) return;

    // get cell_id and search through its hashtable bucket
    uint cell_id = cell_id_from_cell(cell);
    uint hash, bucket_index;
    HashtableEntry entry = { 0, 0, 0, 0, 0 };
    if (hashtable_search(cell_id, entry, hash, bucket_index)) {
        if (!isinf(entry.selected_sample_position.x)) return; // each cell can only contain 1 point at most

        // try pick point from the set of initial sample points
        InitialSamplePoint trial = g_initial_sample_points[entry.initial_sample_point_index + g_sample_gen.trial_index];
        if (cell_id_from_position(trial.position) != cell_id) return; // all points from this cell have been exhausted

        // check placed points in adjacent cells
        for (int x_offset = -2; x_offset <= 2; x_offset++) {
            for (int y_offset = -2; y_offset <= 2; y_offset++) {
                for (int z_offset = -2; z_offset <= 2; z_offset++) {
                    int3 offset = int3(x_offset, y_offset, z_offset);
                    if (all(offset == 0) || all(abs(offset) == 2)) continue;

                    int3 neighbour_cell = int3(cell) + offset;
                    if (any(neighbour_cell < 0) || any(neighbour_cell >= g.grid_dimensions)) continue;

                    HashtableEntry neighbour_entry = { 0, 0, 0, 0, 0 };
                    if (hashtable_search(cell_id_from_cell(neighbour_cell), neighbour_entry)) {
                        float3 d = neighbour_entry.selected_sample_position - trial.position;
                        if (dot(d, d) <= g.rejection_radius*g.rejection_radius) return;
                    }
                }
            }
        }
        // commit new sample point
        uint sample_point_index;
        InterlockedAdd(g_out[0].sample_points_count, 1, sample_point_index);
        if (sample_point_index >= g.sample_points_capacity) return;

        // update hashtable with sample position for neighbouring phase groups (in subsequent dispatches)
        entry.selected_sample_position = trial.position;
        g_hashtable[hash].entries[bucket_index] = entry;

        // create new sample point with world space coordinates and empty payload
        SamplePoint sample_point;
        sample_point.position = mul(float4(trial.position, 1), g.transform).xyz;
        sample_point.payload  = 0;
        g_sample_points[sample_point_index] = sample_point;

        // store world space normal in normals buffer
        Vertex triangle_verts[3] = load_triangle_vertices(g_vertices, load_3x16bit_indices(g_indices, g.indices_offset/3 + trial.triangle_id));
        float3 normal;
        normal = get_interpolated_normal(triangle_verts, get_barycentrics(triangle_verts, trial.position));
        normal = normalize(mul(float4(normal, 0), g.transform)).xyz;
        g_point_normals[sample_point_index] = normal;
    }
}
