#include "types.h"

#include "random.hlsl"

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {
    "DescriptorTable(UAV(u0, numDescriptors = 3))," // 0: { g_render_target, g_sample_accumulator, g_translucent_samples_buffer }
    "CBV(b0)," // 1: g
    "SRV(t0)," // 2: g_scene
    "DescriptorTable(SRV(t1, numDescriptors = 2))," // 3: { g_vertices, g_indices }
};

RWTexture2D<float4> g_render_target       : register(u0);
RWTexture2D<float4> g_sample_accumulator  : register(u1);

// TODO: optimized data structure
RWStructuredBuffer<SamplePoint> g_translucent_samples_buffer : register(u2);

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
    "RootConstants(b1, num32BitConstants = 3)," // 0: l
};

ConstantBuffer<RaytracingLocals> l : register(b1);

// PIPELINE CONFIGURATION

struct RayPayload {
    uint   rng;
    float  t;
    float3 scatter;
    float3 reflectance;
    float3 emission;
};

typedef BuiltInTriangleIntersectionAttributes Attributes;

RaytracingShaderConfig shader_config = {
    44, // max payload size
    8   // max attribute size
};

RaytracingPipelineConfig pipeline_config = {
    1   // max recursion depth
};

// SHADER CODE

float4 trace_path_sample(inout uint rng, inout RayDesc ray) {
    float4 result = 0;

    RayPayload payload;
    payload.rng = rng;

    float3 radiance    = 0;
    float3 reflectance = 1;
    for (uint bounce_index = 0; bounce_index <= g.bounces_per_sample; bounce_index++) {
        TraceRay(
            g_scene, RAY_FLAG_NONE, 0xff,
            0, 1, 0,
            ray, payload
        );
        radiance    += payload.emission * reflectance;
        reflectance *= payload.reflectance;

        if (!any(payload.reflectance)) {
            result.rgb = radiance;
            result.a   = 1;
            if (bounce_index == 0 && isinf(payload.t)) result.a = 0;
            break;
        }

        ray.Origin   += payload.t * ray.Direction;
        ray.Direction = payload.scatter;
    }

    rng = payload.rng; // write rng back out
    return result;
}

[shader("raygeneration")]
void camera_rgen() {
    uint rng = hash(float3(DispatchRaysIndex().xy, g.frame_rng*(g.accumulator_count != 0)));

    RayDesc ray;
    ray.TMin = 0.0001;
    ray.TMax = 10000;

    // accumulate new samples for this frame
    float4 accumulated_samples = 0;

    for (uint sample_index = 0; sample_index < g.samples_per_pixel; sample_index++) {
        // generate camera ray
        ray.Origin = g.camera_to_world[3].xyz / g.camera_to_world[3].w;

        ray.Direction.xy  = DispatchRaysIndex().xy + 0.5;                               // pixel centers
        ray.Direction.xy += 0.5 * float2(random11(rng), random11(rng)); // random offset inside pixel
        ray.Direction.xy  = 2*ray.Direction.xy / DispatchRaysDimensions().xy - 1;       // normalize to clip coordinates
        ray.Direction.x  *= g.camera_aspect;
        ray.Direction.y  *= -1;
        ray.Direction.z   = -g.camera_focal_length;
        ray.Direction     = normalize(mul(float4(ray.Direction, 0), g.camera_to_world).xyz);

        accumulated_samples += trace_path_sample(rng, ray);
    }
    // add previous frames' samples
    if (g.accumulator_count != 0) {
        // TODO: prevent floating-point accumulators from growing too large
        accumulated_samples += g_sample_accumulator[DispatchRaysIndex().xy];
    }

    // calculate final pixel colour and write output values
    // TODO: better gamma-correction
    float4 pixel_color = sqrt(accumulated_samples / (g.accumulator_count+g.samples_per_pixel));
    g_render_target     [DispatchRaysIndex().xy] = pixel_color;
    g_sample_accumulator[DispatchRaysIndex().xy] = accumulated_samples;
}

TriangleHitGroup lambert_hit_group = {
    "",
    "lambert_chit"
};

[shader("closesthit")]
void lambert_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_vertex_indices(l.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_interpolated_normal(indices, attr.barycentrics);

    payload.scatter     = random_on_hemisphere(payload.rng, normal);
    payload.reflectance = l.color * dot(normal, payload.scatter);
    payload.emission    = 0;
    payload.t           = RayTCurrent();
}

TriangleHitGroup light_hit_group = {
    "",
    "light_chit"
};

[shader("closesthit")]
void light_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_vertex_indices(l.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_interpolated_normal(indices, attr.barycentrics);

    payload.scatter     = 0;
    payload.reflectance = 0;
    payload.emission    = l.color * -dot(normal, WorldRayDirection());
    payload.t       = RayTCurrent();
}

[shader("miss")]
void miss(inout RayPayload payload) {
    payload.scatter     = 0;
    payload.reflectance = 0;
    payload.emission    = 0;
    payload.t           = INFINITY;
}

// translucent materials

[shader("raygeneration")]
void translucent_rgen() {
    uint rng = hash(float3(DispatchRaysIndex().xy, g.accumulator_count));

    SamplePoint sample_point = g_translucent_samples_buffer[DispatchRaysIndex().x];

    RayDesc ray;
    ray.TMin = 0.0001;
    ray.TMax = 10000;

    // point normal is packed into flux for initialization
    float3 normal     = sample_point.flux;
    sample_point.flux = 0;

    for (uint sample_index = 0; sample_index < g.samples_per_pixel; sample_index++) {
        ray.Origin    = sample_point.position;
        ray.Direction = random_on_hemisphere(rng, normal);
        sample_point.flux += trace_path_sample(rng, ray).rgb * dot(ray.Direction, normal);
    }
    sample_point.flux /= g.samples_per_pixel;

    g_translucent_samples_buffer[DispatchRaysIndex().x] = sample_point;
}

TriangleHitGroup translucent_hit_group = {
    "",
    "translucent_chit"
};

[shader("closesthit")]
void translucent_chit(inout RayPayload payload, Attributes attr) {
    if (!g.translucent_samples_count) {
        // translucent_samples_count set to 0 for initialization
        payload.scatter     = 0;
        payload.reflectance = 0;
        payload.emission    = 0;
        payload.t           = RayTCurrent();
        return;
    }

    float3 hit_point = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    float attenuation           = g.translucent_scattering + g.translucent_absorption;
    float mean_free_path        = 1 / attenuation;
    float albedo                = g.translucent_scattering / attenuation;
    float effective_attenuation = sqrt(3 * g.translucent_scattering * g.translucent_absorption);

    float eta     = g.translucent_refraction;
    float fresnel = -1.440/(eta*eta) + 0.710/eta + 0.668 + 0.0636*eta; // diffuse fresnel, not fresnel reflectance?

    float3 diffuse_exitance = 0;
    for (int i = 0; i < g.translucent_samples_count; i++) {
        SamplePoint sample_point = g_translucent_samples_buffer[i];
        float3 offset  = sample_point.position - hit_point;
        float  radius2 = dot(offset, offset);

        // subsurface diffuse light source
        float z_real    = mean_free_path;
        float d_real    = sqrt(radius2 + z_real*z_real);
        float c_real    = z_real * (effective_attenuation + 1/d_real);

        // virtual light source above surface
        float z_virtual = mean_free_path * (1 + 1.25*(1 + fresnel)/(1 - fresnel));
        float d_virtual = sqrt(radius2 + z_virtual*z_virtual);
        float c_virtual = z_virtual * (effective_attenuation + 1/d_virtual);

        // combine real and virtual contributions
        float m_real    = c_real    * exp(-effective_attenuation * d_real)    / (d_real*d_real);
        float m_virtual = c_virtual * exp(-effective_attenuation * d_virtual) / (d_virtual*d_virtual);
        diffuse_exitance += albedo/(2*TAU) * (m_real + m_virtual) * sample_point.flux;
    }
    diffuse_exitance /= g.translucent_samples_count;

    payload.scatter     = 0;
    payload.reflectance = 0;
    payload.emission    = diffuse_exitance / TAU;
    payload.t           = RayTCurrent();
}
