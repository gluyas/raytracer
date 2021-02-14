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
    return asfloat(0x3f800000 | random(seed) >> 9) - 1.0;
}

// random float in the range (-1.0f, 1.0f)
float random11(inout uint seed) {
    uint rand = random(seed);
    return asfloat(asuint(asfloat(0x3f800000 | rand >> 9) - 1.0) | (rand & 0x80000000));
}

// Generate a random integer in the range [lower, upper]
uint random(inout uint seed, uint lower, uint upper) {
    return lower + uint(float(upper - lower + 1) * random01(seed));
}

// Generate a random float in the range [lower, upper)
float random(inout uint seed, float lower, float upper) {
    return lower + (upper - lower)*random01(seed);
}

float2 random_in_circle(inout uint seed) {
    float2 unit;
    float  unit2;
    do {
        unit.x = random11(seed);
        unit.y = random11(seed);
        unit2 = dot(unit, unit);
    } while (unit2 > 1.0);
    return unit;
}

float2 random_on_circle(inout uint seed) {
    float theta = random01(seed)*TAU;
    return float2(cos(theta), sin(theta));
}

float3 random_in_sphere(inout uint seed) {
    float3 unit;
    float  unit2;
    do {
        unit.x = random11(seed);
        unit.y = random11(seed);
        unit.z = random11(seed);
        unit2 = dot(unit, unit);
    } while (unit2 > 1.0);
    return unit;
}

float3 random_on_sphere(inout uint seed) {
    float theta1 = random01(seed)*TAU;
    float theta2 = random01(seed)*TAU;
    float cos1   = cos(theta1);
    return float3(cos1*cos(theta2), cos1*sin(theta2), sin(theta1));
}

float3 random_in_hemisphere(inout uint seed, float3 normal) {
    float3 unit = random_in_sphere(seed);
    return unit - min(0.0, 2.0*dot(normal, unit))*normal;
}

float3 random_on_hemisphere(inout uint seed, float3 normal) {
    float3 unit = random_on_sphere(seed);
    return unit - min(0.0, 2.0*dot(normal, unit))*normal;
}
