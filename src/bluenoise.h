#pragma once
#include "prelude.h"

struct BluenoiseComputeGlobals {
    COMMON_UINT   current_phase_group;

    COMMON_FLOAT3 grid_origin;
    COMMON_FLOAT  grid_cell_width;
    COMMON_UINT3  grid_dimensions;
};

#ifdef CPP
namespace Bluenoise {

void init(ID3D12Device* device);

ID3D12Resource* generate_sample_points(
    ID3D12Resource* vertices,
    ID3D12Resource* indices, UINT64 indices_offset,
    float mean_distance
);

}
#endif
