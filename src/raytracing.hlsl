#include "prelude.h"

#include "random.hlsl"

// SHADER RESOURCES

GlobalRootSignature global_root_signature = {    // SLOT : RESOURCE
    "CBV(b0),"                                      // 0 : g
    "DescriptorTable(UAV(u0, numDescriptors = 2))," // 1 : { g_render_target, g_sample_accumulator }
    "SRV(t0),"                                      // 2 : g_scene

    // translucent materials
    "DescriptorTable("                              // 3 : {
        "SRV(t3, numDescriptors = 2),"              //     g_translucent_bssrdf, g_translucent_properties,
        "SRV(t6, numDescriptors = unbounded,"       //     g_translucent_samples
            "flags = DESCRIPTORS_VOLATILE)"
    "),"                                            // }

    "DescriptorTable("                              // 4: {
        "SRV(t5, numdescriptors = 1),"              //     g_write_translucent_indices
        "UAV(u5, numDescriptors = unbounded,"       //     g_write_translucent_samples -> [2*i + 0]
            "offset = 1,"
            "flags = DESCRIPTORS_VOLATILE),"
        "SRV(t0, space=1, numDescriptors = unbounded,"  // g_point_normals             -> [2*i + 1]
            "offset = 1,"
            "flags = DESCRIPTORS_VOLATILE)"
    "),"                                            // }

    // static samplers
    "StaticSampler(s0, addressU=TEXTURE_ADDRESS_BORDER, borderColor=STATIC_BORDER_COLOR_OPAQUE_BLACK)," // BssrdfSampler
};

ConstantBuffer<RaytracingGlobals> g                    : register(b0);
RaytracingAccelerationStructure   g_scene              : register(t0);
RWTexture2D<float4>               g_render_target      : register(u0);
RWTexture2D<float4>               g_sample_accumulator : register(u1);

// TODO: optimized data structure
Texture1D<float3>                       g_translucent_bssrdf          : register(t3);
StructuredBuffer<TranslucentProperties> g_translucent_properties      : register(t4);
// TODO: merge into large buffers for all translucent instances
StructuredBuffer<SamplePoint>           g_translucent_samples[]       : register(t6);

StructuredBuffer<uint>                  g_write_translucent_indices   : register(t5);
RWStructuredBuffer<SamplePoint>         g_write_translucent_samples[] : register(u5);
Buffer<float3>                          g_point_normals[]             : register(t0, space1);

sampler BssrdfSampler : register(s0);

LocalRootSignature local_root_signature = {
    "RootConstants(b1, num32BitConstants = 4)," // 0: l
    "SRV(t1),"                                  // 1: l_vertices
    "SRV(t2),"                                  // 2: l_indices
};

StructuredBuffer<Vertex>         l_vertices : register(t1);
ByteAddressBuffer                l_indices  : register(t2);
ConstantBuffer<RaytracingLocals> l          : register(b1);

inline float3 get_world_space_normal(uint3 triangle_indices, float2 barycentrics) {
    float3 normal;
    normal  = get_interpolated_normal(load_triangle_vertices(l_vertices, triangle_indices), barycentrics);
    normal  = mul(float4(normal, 0), ObjectToWorld4x3());
    normal *= -sign(dot(WorldRayDirection(), normal)); // ensure normal pointing towards viewer (dot < 0)
    normal  = normalize(normal);
    return normal;
}

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
#define IGNORE_TRANSLUCENT_EMISSION true
float4 trace_path_sample(inout uint rng, inout RayDesc ray, bool ignore_translucent_emission = false) {
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
        if (ignore_translucent_emission) {
            // HACK: currently only translucent materials can return positive reflectance and emission
            // ignore translucent emission by checking if both are positive
            if (any(payload.emission) & any(payload.reflectance)) payload.emission = 0;
        }
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
    uint rng = hash(uint3(DispatchRaysIndex().xy, g.frame_rng*(g.accumulator_count != 0)));

    RayDesc ray;
    ray.TMin = 0.0001;
    ray.TMax = 10000;

    // accumulate new samples for this frame
    float4 accumulated_samples = 0;

    for (uint sample_index = 0; sample_index < g.samples_per_pixel; sample_index++) {
        // generate camera ray
        ray.Origin = g.camera_to_world[3].xyz / g.camera_to_world[3].w;

        ray.Direction.xy  = DispatchRaysIndex().xy + 0.5;                         // pixel centers
        ray.Direction.xy += 0.5 * float2(random11(rng), random11(rng));           // random offset inside pixel
        ray.Direction.xy  = 2*ray.Direction.xy / DispatchRaysDimensions().xy - 1; // normalize to clip coordinates
        ray.Direction.x  *= g.camera_aspect;
        ray.Direction.y  *= -1;
        ray.Direction.z   = -g.camera_focal_length;
        ray.Direction     = normalize(mul(float4(ray.Direction, 0), g.camera_to_world).xyz);

        accumulated_samples += trace_path_sample(rng, ray);
    }
    accumulated_samples /= g.samples_per_pixel;
    // add previous frames' samples
    if (g.accumulator_count != 0) {
        // TODO: prevent floating-point accumulators from growing too large
        accumulated_samples += g_sample_accumulator[DispatchRaysIndex().xy];
    }

    // calculate final pixel colour and write output values
    g_render_target     [DispatchRaysIndex().xy] = sqrt(accumulated_samples / (g.accumulator_count+1)); // TODO: better gamma-correction
    g_sample_accumulator[DispatchRaysIndex().xy] = accumulated_samples;
}

TriangleHitGroup lambert_hit_group = {
    "",
    "lambert_chit"
};

[shader("closesthit")]
void lambert_chit(inout RayPayload payload, Attributes attr) {
    uint3  indices = load_3x16bit_indices(l_indices, PrimitiveIndex());
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
    uint3  indices = load_3x16bit_indices(l_indices, PrimitiveIndex());
    float3 normal  = get_world_space_normal(indices, attr.barycentrics);

    payload.scatter     = 0;
    payload.reflectance = 0;
    payload.emission    = l.color * -dot(normal, WorldRayDirection());
    payload.t           = RayTCurrent();
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
    float r0 = (refractive_index - 1) / (refractive_index + 1);
    r0 *= r0;

    float fresnel;
    fresnel  = 1 - cosine;
    fresnel *= fresnel*fresnel*fresnel*fresnel;
    fresnel *= 1 - r0;
    fresnel += r0;

    return fresnel;
}

[shader("raygeneration")]
// x = sample point index
// y = translucent_id + instance_id*translucent_instance_stride
void translucent_rgen() {
    uint2 index = DispatchRaysIndex().xy;
    index.y = g_write_translucent_indices[index.y];

    // fetch sample point data
    RWStructuredBuffer<SamplePoint> sample_points = g_write_translucent_samples[2*index.y + 0];
    {
        // TODO: don't discard shader threads
        uint samples_count, _stride;
        sample_points.GetDimensions(samples_count, _stride);
        if (index.x >= samples_count) return;
    }
    TranslucentProperties translucent  = g_translucent_properties[index.y];
    SamplePoint           sample_point = sample_points[index.x];
    float3                normal       = g_point_normals[2*index.y + 1][index.x];

    if (g.translucent_accumulator_count == 0) sample_point.payload = 0;

    // accumulate irradiance samples
    uint rng = hash(uint3(index, g.frame_rng*(g.translucent_accumulator_count != 0)));

    RayDesc ray;
    ray.TMin = 0.0001;
    ray.TMax = 10000;

    float3 transmitted_irradiance = 0;
    for (uint i = 0; i < g.samples_per_pixel; i++) {
        ray.Origin    = sample_point.position;
        ray.Direction = random_on_hemisphere(rng, normal); // TODO: cosine weighted samples

        float3 radiance = trace_path_sample(rng, ray, IGNORE_TRANSLUCENT_EMISSION).rgb; // ignore translucent emission to prevent positive feedback
        float  cosine   = -dot(ray.Direction, normal);
        float  fresnel  = 1 - schlick(g.translucent_refractive_index, cosine);

        transmitted_irradiance += radiance * cosine * fresnel;
    }
    sample_point.payload += (transmitted_irradiance * translucent.samples_mean_area) / (TAU/2 * g.samples_per_pixel);
    sample_points[index.x] = sample_point;
}

TriangleHitGroup translucent_hit_group = {
    "",
    "translucent_chit"
};

inline float3 eval_bssrdf_tabulated(TranslucentProperties translucent, float radius) {
    float z = g.translucent_bssrdf_scale*g.translucent_bssrdf_scale;
    float s = g.translucent_bssrdf_fudge;
    return s * g_translucent_bssrdf.SampleLevel(BssrdfSampler, radius / g.translucent_bssrdf_scale, 0) / z;
}

inline float3 eval_bssrdf_dipole(TranslucentProperties translucent, float radius) {
    float3 attenuation           = g.translucent_scattering + g.translucent_absorption;
    float3 mean_free_path        = 1 / attenuation;
    float3 albedo                = g.translucent_scattering / attenuation;
    float3 effective_attenuation = sqrt(3 * g.translucent_scattering * g.translucent_absorption);

    float eta              = g.translucent_refractive_index;
    float diffuse_fresnel = -1.440/(eta*eta) + 0.710/eta + 0.668 + 0.0636*eta;

    // subsurface diffuse light source
    float3 z_real    = mean_free_path;
    float3 d_real    = radius + z_real;
    float3 c_real    = z_real * (effective_attenuation + 1/d_real);

    // virtual light source above surface
    float3 z_virtual = mean_free_path * (1 + 1.25*(1 + diffuse_fresnel)/(1 - diffuse_fresnel));
    float3 d_virtual = radius + z_virtual;
    float3 c_virtual = z_virtual * (effective_attenuation + 1/d_virtual);

    // combine real and virtual contributions
    float3 m_real    = c_real    * exp(-effective_attenuation * d_real)    / (d_real*d_real);
    float3 m_virtual = c_virtual * exp(-effective_attenuation * d_virtual) / (d_virtual*d_virtual);
    return max(0, albedo/(2*TAU) * (m_real + m_virtual));
}

void debug_draw_translucent_samples(inout RayPayload payload, Attributes attr);

#define TRANSLUCENT_INIT() \
    uint index = l.translucent_id * g.translucent_instance_stride + InstanceID(); \
    TranslucentProperties         translucent   = g_translucent_properties[index]; \
    StructuredBuffer<SamplePoint> samples       = g_translucent_samples[index]; \
    uint samples_count, _stride; samples.GetDimensions(samples_count, _stride); \

[shader("closesthit")]
void translucent_chit(inout RayPayload payload, Attributes attr) {
    // debug_draw_translucent_samples(payload, attr); return; // debug visualisation of sample points
    TRANSLUCENT_INIT();

    uint3  indices = load_3x16bit_indices(l_indices, PrimitiveIndex());
    float3 normal  = get_world_space_normal(indices, attr.barycentrics);
    // calculated in model space to match sample points
    float3 hit_point = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    float3 diffuse_irradiance = 0;
    if (g.translucent_accumulator_count && g.translucent_bssrdf_fudge) {
        for (int i = 0; i < samples_count; i++) {
            SamplePoint sample_point = samples[i];
            float radius = length(sample_point.position - hit_point);
            float3 bssrdf;
            if (g.translucent_bssrdf_scale) bssrdf = eval_bssrdf_tabulated(translucent, radius);
            else                            bssrdf = eval_bssrdf_dipole(translucent, radius);

            diffuse_irradiance += bssrdf * sample_point.payload;
        }
        diffuse_irradiance /= g.translucent_accumulator_count;
    }

    float  n = g.translucent_refractive_index;
    float3 scatter = random_on_hemisphere(payload.rng, normal);
    float  lambert = dot(scatter, normal);

    float  incident_cosine     = lambert; // = dot(scatter, normal);
    float  incident_fresnel    = schlick(n, incident_cosine);                          // boundary n1=1, n2>1; reflected component

    float  transmitted_cosine  = sqrt(1 - 1/(n*n)*(1 - -dot(WorldRayDirection(), normal))); // nested identity: cos(asin(x)) = sin(acos(x)) = sqrt(1-x^2)
    float  transmitted_fresnel = 1 - schlick(n, transmitted_cosine);                   // boundary n1>1, n2=1; transmitted component

    payload.scatter     = scatter;
    payload.reflectance = l.color * lambert * incident_fresnel;
    payload.emission    = diffuse_irradiance * transmitted_fresnel / (TAU/2);
    payload.t           = RayTCurrent();
}

// DEBUG HELPERS

void debug_draw_translucent_samples(inout RayPayload payload, Attributes attr) {
    TRANSLUCENT_INIT();

    payload.scatter = 0;
    payload.reflectance = 0;
    payload.emission = 0;
    payload.t = RayTCurrent();

    const float rejection_radius = 0.05;
    // const float grid_cell_width = rejection_radius / sqrt(3);
    // const float3 grid_origin = -0.15299781;
    // const uint3  grid_dimensions = 53;

    float3 hit = WorldRayOrigin() + RayTCurrent()*WorldRayDirection();

    // float3 cell = floor((hit - grid_origin) / grid_cell_width) % 3;
    // payload.emission = cell / 2;

    float min_d = INFINITY;
    float3 color = 0;
    for (int i = samples_count-1; i >= 0; i--) {
        SamplePoint sample_point = samples[i];

        float3 d = sample_point.position - hit;
        // if (dot(d, d) <= rejection_radius*rejection_radius*0.25) {
        // if (dot(d, d) <= 0.000001) {
            // // payload.emission = 1;
            // // payload.emission = abs(sample_point.payload);
            // // payload.emission = length(payload.emission) > 0.5*sqrt(2)? 0 : 1;
            // return;
        // }
        if (length(d) < min_d) {
            min_d = length(d);
            color = sample_point.payload;
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
    float p = max(0.0, g.translucent_bssrdf_scale - min_d) / g.translucent_bssrdf_scale;
    payload.emission = p * color;
    if (abs(p - 0.5) < 0.05) payload.emission = 1 - payload.emission;
}
