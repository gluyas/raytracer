#include "prelude.h"
// #include "geometry.h"

namespace Bluenoise {

#include "out/bluenoise.hlsl.h"

// TODO: descriptor layout
#define DESCRIPTORS_COUNT 2

ID3D12DescriptorHeap* g_descriptor_heap = NULL;
UINT                  g_descriptor_size = 0;

ID3D12RootSignature* g_root_signature = NULL;
ID3D12PipelineState* g_pipeline_state = NULL;

void init(ID3D12Device* device) {
    { // g_descriptor_heap
        D3D12_DESCRIPTOR_HEAP_DESC g_descriptor_heap_desc = {};
        g_descriptor_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        g_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_descriptor_heap_desc.NumDescriptors = DESCRIPTORS_COUNT;
        CHECK_RESULT(device->CreateDescriptorHeap(&g_descriptor_heap_desc, IID_PPV_ARGS(&g_descriptor_heap)));
    }
    g_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // load shaders & root signatures
    CHECK_RESULT(device->CreateRootSignature(0, g_bluenoise_hlsl_bytecode, _countof(g_bluenoise_hlsl_bytecode), IID_PPV_ARGS(&g_root_signature)));

    { // g_pipeline_state
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = g_root_signature;
        desc.CS             = CD3DX12_SHADER_BYTECODE((void*) g_bluenoise_hlsl_bytecode, _countof(g_bluenoise_hlsl_bytecode));
        CHECK_RESULT(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_pipeline_state)));
    }
}

ID3D12Resource* generate_sample_points(
    // command list to submit to?
    ID3D12Resource* vertices,
    ID3D12Resource* indices, UINT64 indices_offset,
    float mean_distance
) {
    return NULL;
}

/*
// find index of smallest float greater than or equal to value
size_t binary_search_float(ArrayView<float> ascending_floats, float value) {
    size_t min_index = 0;
    size_t max_index = ascending_floats.len-1;

    while (min_index != max_index) {
        size_t i = (min_index + max_index) / 2;
        if (value > ascending_floats[i]) {
            min_index = i + 1;
        } else {
            max_index = i;
        }
    }
    return min_index;
}

float random();

Array<SamplePoint> generate_sample_points_for_mesh(
    ArrayView<Vertex> vertices,
    ArrayView<Index>  indices,
    float mean_distance
) {
    // record running total of each triangle's area for uniform sampling over mesh surface
    float        total_surface_area       = 0;
    Array<float> cumulative_surface_areas = array_init<float>((indices.len+2)/3);
    for (size_t i = 0; i < indices.len-2; i += 3) {
        total_surface_area += triangle_area(load_triangle_from_3_indices(vertices, &indices[i*3]));
        array_push(&cumulative_surface_areas, total_surface_area);
    }

    // generate uniform random samples over mesh surface
    size_t sample_ponts_count = (size_t) ceilf(total_surface_area / (mean_distance*mean_distance));
    Array<SamplePoint> sample_points                 = array_init<SamplePoint>(sample_ponts_count);
    Array<size_t>      sample_point_triangle_indices = array_init<size_t>     (sample_ponts_count);

    for (size_t i = 0; i < sample_ponts_count; i += 1) {
        // random triangle weighted by surface area
        size_t   triangle_index = binary_search_float(cumulative_surface_areas, random());
        Triangle triangle       = load_triangle_from_3_indices(vertices, &indices[triangle_index*3]);

        SamplePoint sample_point;

        // uniform random point on triangle surface
        float u = sqrt(random());
        float v = u * random();
        u = 1 - u;

        XMVECTOR point = triangle.a;
        point += u * (triangle.b - triangle.a);
        point += v * (triangle.c - triangle.a);
        XMStoreFloat3(&sample_point.position, point);

        // triangle normal in flux field for initialization

        array_push(&sample_point_triangle_indices, triangle_index);
        array_push(&sample_points, sample_point);
    }

    array_free(&cumulative_surface_areas);
    return sample_points;
}

ID3D12Resource* generate_sample_points() {

}
*/
}
