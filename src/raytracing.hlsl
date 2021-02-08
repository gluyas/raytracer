#include "types.h"

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {
    "DescriptorTable(UAV(u0))," // 0: { g_render_target }
    "CBV(b0)," // 1: g
    "SRV(t0)," // 2: g_scene
    "DescriptorTable(SRV(t1, numDescriptors = 2))," // 3: { g_vertices, g_indices }
};

RWTexture2D<float4> g_render_target : register(u0);

ConstantBuffer<RaytracingGlobals> g : register(b0);

RaytracingAccelerationStructure g_scene    : register(t0);
StructuredBuffer<Vertex>        g_vertices : register(t1);
ByteAddressBuffer               g_indices  : register(t2);

inline uint3 load_vertex_indices(uint triangle_index) {
    const uint indices_per_triangle = 3;
    const uint bytes_per_index      = 2;
    const uint align_to_4bytes_mask = ~0x0003;
    uint triangle_byte_index = triangle_index * indices_per_triangle * bytes_per_index;
    uint aligned_byte_index  = triangle_byte_index & align_to_4bytes_mask;

    dword2 four_indices = g_indices.Load2(aligned_byte_index);

    uint3 indices;
    if (triangle_byte_index == aligned_byte_index) {
        // lower three indices
        indices.x = (four_indices.x      ) & 0xffff;
        indices.y = (four_indices.x >> 16) & 0xffff;
        indices.z = (four_indices.y      ) & 0xffff;
    } else {
        // upper three indices
        indices.x = (four_indices.x >> 16) & 0xffff;
        indices.y = (four_indices.y      ) & 0xffff;
        indices.z = (four_indices.y >> 16) & 0xffff;
    }
    return indices;
}

LocalRootSignature local_root_signature = {
    "RootConstants(b1, num32BitConstants = 3)," // 0: l
};

ConstantBuffer<RaytracingLocals> l : register(b1);

// PIPELINE CONFIGURATION

struct RayPayload {
    float3 color;
};

typedef BuiltInTriangleIntersectionAttributes Attributes;

RaytracingShaderConfig shader_config = {
    16, // max payload size
    8   // max attribute size
};

RaytracingPipelineConfig pipeline_config = {
    1   // max recursion depth
};

// TODO: generate hitgroups for each material
TriangleHitGroup hit_group = {
    "",
    "chit"
};

// SHADER CODE

[shader("raygeneration")]
void rgen() {
    RayDesc ray;
    ray.Origin = g.camera_to_world[3].xyz / g.camera_to_world[3].w;

    ray.Direction.xy = 2*(float2(DispatchRaysIndex().xy) + 0.5) / float2(DispatchRaysDimensions().xy) - 1;
    ray.Direction.x *= g.camera_aspect;
    ray.Direction.y *= -1;
    ray.Direction.z = -g.camera_focal_length;

    ray.Direction = normalize(mul(float4(ray.Direction, 0), g.camera_to_world).xyz);

    ray.TMin = 0.0001;
    ray.TMax = 1000;

    RayPayload payload;
    payload.color = 0;
    TraceRay(
        g_scene, RAY_FLAG_NONE, 0xff,
        0, 1, 0,
        ray, payload
    );

    g_render_target[DispatchRaysIndex().xy] = float4(payload.color, 1);
}

[shader("closesthit")]
void chit(inout RayPayload payload, Attributes attr) {
    // uint3 indices = load_vertex_indices(PrimitiveIndex());

    // float3 normal_x = abs(g_vertices[indices.x].normal);
    // payload.color  = normal_x;
    // payload.color += attr.barycentrics.x * (abs(g_vertices[indices.y].normal) - normal_x);
    // payload.color += attr.barycentrics.y * (abs(g_vertices[indices.z].normal) - normal_x);
    payload.color = l.color / RayTCurrent();
}

[shader("miss")]
void miss(inout RayPayload payload) {
    payload.color = 0;
}
