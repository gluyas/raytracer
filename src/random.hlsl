#pragma once

#include "types.h"

// http://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/

uint hash(uint seed) {
    // Thomas Wang hash
    // http://www.burtleburtle.net/bob/hash/integer.html
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

uint hash(uint2 seed) {
    return hash(seed.y) + 31*hash(seed.x);
}

uint hash(uint3 seed) {
    return hash(seed.z) + 31*hash(seed.xy);
}

uint hash(uint4 seed) {
    return hash(seed.w) + 31*hash(seed.xyz);
}

// random 32-bit integer
uint random(inout uint seed) {
    // Xorshift algorithm from George Marsaglia's paper.
    seed ^= (seed << 13);
    seed ^= (seed >> 17);
    seed ^= (seed << 5);
    return seed;
}

// random float in the range [0.0f, 1.0f)
float random01(inout uint seed) {
    return asfloat(0x3f800000 | (0x007fffff & random(seed))) - 1;
}

// random float in the range (-1.0f, 1.0f)
float random11(inout uint seed) {
    uint  rand   = random(seed);
    float rand01 = asfloat(0x3f800000 | (0x007fffff & rand)) - 1;
    return asfloat(asuint(rand01) | (rand & 0x80000000));
}

// Generate a random integer in the range [lower, upper]
uint random(inout uint seed, uint lower, uint upper) {
    return lower + uint(float(upper - lower + 1) * random01(seed));
}

// Generate a random float in the range [lower, upper)
float random(inout uint seed, float lower, float upper) {
    return lower + (upper - lower)*random01(seed);
}

float2 random_on_circle(inout uint seed) {
    float theta = random01(seed)*TAU;
    return float2(cos(theta), sin(theta));
}

float2 random_inside_circle(inout uint seed) {
    float r = sqrt(random01(seed));
    return r * random_on_circle(seed);
}

float3 random_on_sphere(inout uint seed) {
    float phi       = random01(seed)*TAU;
    float cos_theta = random11(seed);
    float sin_theta = sqrt(1 - cos_theta*cos_theta);
    return float3(sin_theta*cos(phi), sin_theta*sin(phi), cos_theta);
}

float3 random_inside_sphere(inout uint seed) {
    float3 unit;
    do {
        unit = float3(random11(seed), random11(seed), random11(seed));
    } while (dot(unit, unit) > 1);
    return unit;
}

float3 random_on_hemisphere(inout uint seed, float3 normal) {
    float3 unit = random_on_sphere(seed);
    return unit - min(0.0, 2.0*dot(normal, unit))*normal;
}

float3 random_inside_hemisphere(inout uint seed, float3 normal) {
    float3 unit = random_inside_sphere(seed);
    return unit - min(0.0, 2.0*dot(normal, unit))*normal;
}
