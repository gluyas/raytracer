// TODO: single definition of common structures
struct Vertex {
    float3 position;
    float3 color;
};

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {
    "DescriptorTable(UAV(u0))," // 0: { g_render_target }
    "SRV(t0)," // 1: g_scene
    "DescriptorTable(SRV(t1, numDescriptors = 2))," // 2: { g_vertices, g_indices }
};

RWTexture2D<float4> g_render_target : register(u0);

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
    ray.Direction  = float3(0, 0, 1);
    ray.Origin     = float3(2*float2(DispatchRaysIndex().xy) / float2(DispatchRaysDimensions().xy), -1);
    ray.Origin.xy += -1;
    ray.Origin.y  *= -1;

    ray.TMin = 0.0001;
    ray.TMax = 1000;

    RayPayload payload;
    payload.color = 0;
    TraceRay(
        g_scene, RAY_FLAG_NONE, 0xff,
        0, 0, 0,
        ray, payload
    );

    g_render_target[DispatchRaysIndex().xy] = float4(payload.color, 1);
}

[shader("closesthit")]
void chit(inout RayPayload payload, Attributes attr) {
    uint3 indices = load_vertex_indices(PrimitiveIndex());

    float3 color_x = g_vertices[indices.x].color;
    payload.color  = color_x;
    payload.color += attr.barycentrics.x * (g_vertices[indices.y].color - color_x);
    payload.color += attr.barycentrics.y * (g_vertices[indices.z].color - color_x);
}

[shader("miss")]
void miss(inout RayPayload payload) {
    payload.color = 0;
}
