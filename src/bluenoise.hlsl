#include "prelude.h"

#include "bluenoise.h"

BluenoiseComputeGlobals g;

StructuredBuffer<Vertex> g_vertices;
ByteAddressBuffer        g_indices;

struct InitialSamplePoint {
    float3 position;
    uint   triangle_id;
};
RWStructuredBuffer<InitialSamplePoint> g_inital_sample_points;

struct HashTableEntry {
    uint cell_id;    // key
    uint cell_index; // index into g_inital_sample_points
};

#define HASH_TABLE_BUCKET_SIZE 4 // TODO: make into root argument?
struct HashTableBucket {
    uint           entries_count;
    HashTableEntry entries[HASH_TABLE_BUCKET_SIZE];
};

StructuredBuffer<float> g_partial_surface_areas;
RWStructuredBuffer<HashTableBucket> g_hash_table;

RWStructuredBuffer<SamplePoint> g_final_sample_points;

void generate_initial_sample_points() {

}

// determine cell grid bounds and resolution on host

void sort_initial_sample_points_by_cell_id() {

}

void build_hash_table() {

}

// determine phase group order on host
// iteratively call this kernel until enough points have been generated

void try_put_random_point_in_phase_group_cells() {
    uint3 phase_group;
    phase_group.x = (g.current_phase_group / 1) % 3;
    phase_group.y = (g.current_phase_group / 3) % 3;
    phase_group.z = (g.current_phase_group / 9) % 3;

    // get cell_id for phase group and dispatch index
}
