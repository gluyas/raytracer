#include "prelude.h"

#include "random.hlsl"

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {
    "DescriptorTable(UAV(u0, numDescriptors = 3))," // 0: { g_render_target, g_sample_accumulator, g_translucent_samples }
    "CBV(b0)," // 1: g
    "SRV(t0)," // 2: g_scene
    "DescriptorTable(SRV(t1, numDescriptors = 2))," // 3: { g_vertices, g_indices }
};

RWTexture2D<float4> g_render_target       : register(u0);
RWTexture2D<float4> g_sample_accumulator  : register(u1);

// TODO: optimized data structure
RWStructuredBuffer<SamplePoint> g_translucent_samples : register(u2);

ConstantBuffer<RaytracingGlobals> g : register(b0);

RaytracingAccelerationStructure g_scene    : register(t0);
StructuredBuffer<Vertex>        g_vertices : register(t1);
ByteAddressBuffer               g_indices  : register(t2);

inline float3 get_world_space_normal(uint3 triangle_indices, float2 barycentrics) {
    float3 normal;
    normal  = get_interpolated_normal(load_triangle_vertices(g_vertices, triangle_indices), barycentrics);
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
    uint bounce_index;
    for (bounce_index = 0; bounce_index <= g.bounces_per_sample; bounce_index++) {
        TraceRay(
            g_scene, RAY_FLAG_NONE, 0xff,
            0, 1, 0,
            ray, payload
        );
        radiance    += payload.emission * reflectance;
        reflectance *= payload.reflectance;

        if (!any(payload.reflectance)) break;

        ray.Origin   += payload.t * ray.Direction;
        ray.Direction = payload.scatter;
    }
    result.rgb = radiance;
    result.a   = !(bounce_index == 0 && isinf(payload.t));

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
    uint3  indices = load_3x16bit_indices(g_indices, l.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_world_space_normal(indices, attr.barycentrics);

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
    uint3  indices = load_3x16bit_indices(g_indices, l.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_world_space_normal(indices, attr.barycentrics);

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

float schlick(float refractive_index, float cosine) {
    float r0 = (1 - g.translucent_refraction) / (1 + g.translucent_refraction);
    r0 *= r0;

    float fresnel;
    fresnel  = 1 - cosine;
    fresnel *= fresnel*fresnel*fresnel*fresnel;
    fresnel *= 1 - r0;
    fresnel += r0;

    return fresnel;
}

[shader("raygeneration")]
void translucent_rgen() {
    uint rng = hash(float3(DispatchRaysIndex().xy, g.accumulator_count));

    SamplePoint sample_point = g_translucent_samples[DispatchRaysIndex().x];

    RayDesc ray;
    ray.TMin = 0.0001;
    ray.TMax = 10000;

    // point normal is packed into payload for initialization
    float3 normal        = sample_point.payload;
    sample_point.payload = 0;

    // store incident flux into sample_point payload field
    for (uint sample_index = 0; sample_index < g.samples_per_pixel; sample_index++) {
        ray.Origin    = sample_point.position;
        ray.Direction = random_on_hemisphere(rng, normal);
        sample_point.payload += trace_path_sample(rng, ray).rgb * dot(ray.Direction, normal);
    }
    sample_point.payload /= g.samples_per_pixel;

    g_translucent_samples[DispatchRaysIndex().x] = sample_point;
}

TriangleHitGroup translucent_hit_group = {
    "",
    "translucent_chit"
};

void debug_draw_translucent_samples(inout RayPayload payload, Attributes attr);

[shader("closesthit")]
void translucent_chit(inout RayPayload payload, Attributes attr) {
    // debug_draw_translucent_samples(payload, attr); return; // debug visualisation of sample points
    uint3  indices = load_3x16bit_indices(g_indices, l.primitive_index_offset + PrimitiveIndex());
    float3 normal  = get_world_space_normal(indices, attr.barycentrics);
    float3 hit_point = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    float3 diffuse_exitance = 0;
    if (g.translucent_samples_count) {
        float attenuation           = g.translucent_scattering + g.translucent_absorption;
        float mean_free_path        = 1 / attenuation;
        float albedo                = g.translucent_scattering / attenuation;
        float effective_attenuation = sqrt(3 * g.translucent_scattering * g.translucent_absorption);

        float eta             = g.translucent_refraction;
        float diffuse_fresnel = -1.440/(eta*eta) + 0.710/eta + 0.668 + 0.0636*eta; // diffuse fresnel, not fresnel reflectance?

        for (int i = 0; i < g.translucent_samples_count; i++) {
            SamplePoint sample_point = g_translucent_samples[i];
            float3 offset  = sample_point.position - hit_point;
            float  radius2 = dot(offset, offset);

            // subsurface diffuse light source
            float z_real    = mean_free_path;
            float d_real    = sqrt(radius2 + z_real*z_real);
            float c_real    = z_real * (effective_attenuation + 1/d_real);

            // virtual light source above surface
            float z_virtual = mean_free_path * (1 + 1.25*(1 + diffuse_fresnel)/(1 - diffuse_fresnel));
            float d_virtual = sqrt(radius2 + z_virtual*z_virtual);
            float c_virtual = z_virtual * (effective_attenuation + 1/d_virtual);

            // combine real and virtual contributions
            float m_real    = c_real    * exp(-effective_attenuation * d_real)    / (d_real*d_real);
            float m_virtual = c_virtual * exp(-effective_attenuation * d_virtual) / (d_virtual*d_virtual);
            diffuse_exitance += max(0, albedo/(2*TAU) * (m_real + m_virtual) * sample_point.payload);
        }
        diffuse_exitance /= g.translucent_samples_count;
    }

    float3 scatter = random_on_hemisphere(payload.rng, normal);
    float  cosine  = dot(scatter, normal);
    float  fresnel = schlick(g.translucent_refraction, cosine);

    payload.scatter     = scatter;
    payload.reflectance = cosine * fresnel;
    payload.emission    = diffuse_exitance / TAU;
    payload.t           = RayTCurrent();
}

// DEBUG HELPERS

void debug_draw_translucent_samples(inout RayPayload payload, Attributes attr) {
    payload.scatter = 0;
    payload.reflectance = 0;
    payload.emission = 0;
    payload.t = RayTCurrent();

    const float rejection_radius = 0.01;
    const float grid_cell_width = rejection_radius / sqrt(3);
    const float3 grid_origin = -0.15299781;
    const uint3  grid_dimensions = 53;

    float3 hit = WorldRayOrigin() + RayTCurrent()*WorldRayDirection();

    float3 cell = floor((hit - grid_origin) / grid_cell_width) % 3;
    // payload.emission = cell / 2;

    for (int i = g.translucent_samples_count-1; i >= 0; i--) {
        SamplePoint sample_point = g_translucent_samples[i];

        float3 d = sample_point.position - hit;
        if (dot(d, d) <= rejection_radius*rejection_radius*0.25) {
        // if (dot(d, d) <= 0.000001) {
            payload.emission = 1;
            // payload.emission = length(payload.emission) > 0.5*sqrt(2)? 0 : 1;
            return;
        }

        // float3 m = WorldRayOrigin() - sample_point.position;
        // float  b = dot(m, WorldRayDirection());
        // float  c = dot(m, m) - rejection_radius*rejection_radius*0.25;
        // if (c > 0 && b > 0) continue;

        // float d = b*b - c;
        // if (d < 0) continue;

        // float e = sqrt(d);
        // float t = -b - e;
        // if (t >= 0) {
        //     payload.emission = 1;
        //     return;
        // }
    }
}
