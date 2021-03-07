#include "types.h"

#include "random.hlsl"

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {
    "DescriptorTable(UAV(u0, numDescriptors = 2))," // 0: { g_render_target, g_sample_accumulator }
    "CBV(b0)," // 1: g_
    "SRV(t0)," // 2: g_scene
    "DescriptorTable(SRV(t1, numDescriptors = 2))," // 3: { g_vertices, g_indices }
};

RWTexture2D<float4> g_render_target       : register(u0);
RWTexture2D<float4> g_sample_accumulator  : register(u1);

ConstantBuffer<RaytracingGlobals> g_ : register(b0);

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

inline float3 get_interpolated_normal(uint3 vertex_indices, float2 barycentrics) {
    float3 normal;
    float3 normal_x = g_vertices[vertex_indices.x].normal;
    normal  = normal_x;
    normal += barycentrics.x * (g_vertices[vertex_indices.y].normal - normal_x);
    normal += barycentrics.y * (g_vertices[vertex_indices.z].normal - normal_x);
    normal  = mul(float4(normal, 0), ObjectToWorld4x3());
    normal *= -sign(dot(WorldRayDirection(), normal));
    normal  = normalize(normal);
    return normal;
}

LocalRootSignature local_root_signature = {
    "RootConstants(b1, num32BitConstants = 3)," // 0: l_
};

ConstantBuffer<RaytracingLocals> l_ : register(b1);

// PIPELINE CONFIGURATION

struct RayPayload {
    uint   rng;
    float  t;
    float3 scatter;
    float3 color;
};

typedef BuiltInTriangleIntersectionAttributes Attributes;

RaytracingShaderConfig shader_config = {
    32, // max payload size
    8   // max attribute size
};

RaytracingPipelineConfig pipeline_config = {
    1   // max recursion depth
};

// SHADER CODE

[shader("raygeneration")]
void rgen() {
    RayPayload payload;
    payload.rng = hash(float3(DispatchRaysIndex().xy, g_.accumulator_count));

    RayDesc ray;

    ray.TMin = 0.0001;
    ray.TMax = 10000;

    // accumulate new samples for this frame
    float4 accumulated_samples = float4(0, 0, 0, g_.samples_per_pixel);

    for (uint sample_index = 0; sample_index < g_.samples_per_pixel; sample_index++) {
        ray.Origin = g_.camera_to_world[3].xyz / g_.camera_to_world[3].w;

        ray.Direction.xy  = DispatchRaysIndex().xy + 0.5;                               // pixel centers
        ray.Direction.xy += 0.5 * float2(random11(payload.rng), random11(payload.rng)); // random offset inside pixel
        ray.Direction.xy  = 2*ray.Direction.xy / DispatchRaysDimensions().xy - 1;       // normalize to clip coordinates
        ray.Direction.x  *= g_.camera_aspect;
        ray.Direction.y  *= -1;
        ray.Direction.z   = -g_.camera_focal_length;
        ray.Direction     = normalize(mul(float4(ray.Direction, 0), g_.camera_to_world).xyz);

        float3 sample_color = 1;
        for (uint bounce_index = 0; bounce_index <= g_.bounces_per_sample; bounce_index++) {
            TraceRay(
                g_scene, RAY_FLAG_NONE, 0xff,
                0, 1, 0,
                ray, payload
            );
            sample_color *= payload.color;

            if (!any(payload.scatter)) {
                accumulated_samples.rgb += sample_color;
                if (bounce_index == 0 && isinf(payload.t)) {
                    // subtract from alpha channel if primary ray misses geometry
                    accumulated_samples.a -= 1;
                }
                break;
            }
            ray.Origin   += ray.Direction * payload.t;
            ray.Direction = payload.scatter;
        }
    }
    // add previous frames' samples
    if (g_.accumulator_count != 0) {
        // TODO: prevent floating-point accumulators from growing too large
        accumulated_samples += g_sample_accumulator[DispatchRaysIndex().xy];
    }

    // calculate final pixel colour and write output values
    // TODO: better gamma-correction
    float4 pixel_color = sqrt(accumulated_samples / (g_.accumulator_count+g_.samples_per_pixel));
    g_render_target     [DispatchRaysIndex().xy] = pixel_color;
    g_sample_accumulator[DispatchRaysIndex().xy] = accumulated_samples;
}

TriangleHitGroup lambert_hit_group = {
    "",
    "lambert_chit"
};

[shader("closesthit")]
void lambert_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_vertex_indices(l_.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_interpolated_normal(indices, attr.barycentrics);

    payload.scatter = random_on_hemisphere(payload.rng, normal);
    payload.color   = l_.color * dot(normal, payload.scatter);
    payload.t       = RayTCurrent();
}

TriangleHitGroup cook_torrance_hit_group = {
    "",
    "cook_torrance_chit"
};

[shader("closesthit")]
void cook_torrance_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_vertex_indices(l_.primitive_index_offset + PrimitiveIndex());

    float3 n = get_interpolated_normal(indices, attr.barycentrics); // surface normal
    float3 v = -WorldRayDirection();                                // viewer direction
    float3 l = random_on_hemisphere(payload.rng, n);                // light direction
    float3 h = normalize(v + l);                                    // half vector

    float n_l = dot(n, l);
    float n_v = dot(n, v);
    float n_h = dot(n, h);
    float v_h = dot(v, h);

    float d; { // microfacet normal coefficient
        // beckmann distribution
        float cos_alpha  = dot(n, h);
        float cos_alpha2 = cos_alpha*cos_alpha;
        float tan_alpha  = sqrt(1 - cos_alpha2) / cos_alpha;
        float rm2        = 1 / (g_.debug_roughness*g_.debug_roughness);
        d = exp(-tan_alpha*tan_alpha * rm2) * rm2 / (cos_alpha2*cos_alpha2);
    }

    float g; { // geometry coefficient (masking & shadowing)
        // cook torrance
        g = min(1, (2 * n_h * min(n_v, n_l)) / (v_h));
    }

    float f; { // fresnel coefficient
        // schlick approximation
        f = 1 - v_h;
        f *= (f*f)*(f*f);
        f *= (1 - g_.debug_f0);
        f += g_.debug_f0;
    }

    float specular = (f * d * g) / (4*n_l*n_v);
    float lambert  = n_l;

    payload.scatter = l;
    payload.color   = min(1, specular*g_.debug_rs + lambert*g_.debug_rl);
    payload.t       = RayTCurrent();
}

TriangleHitGroup light_hit_group = {
    "",
    "light_chit"
};

[shader("closesthit")]
void light_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_vertex_indices(l_.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_interpolated_normal(indices, attr.barycentrics);

    payload.scatter = 0;
    payload.color   = l_.color * -dot(normal, WorldRayDirection());
    payload.t       = RayTCurrent();
}

[shader("miss")]
void miss(inout RayPayload payload) {
    payload.color   = 0;
    payload.scatter = 0;
    payload.t       = INFINITY;
}
