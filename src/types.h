#define TAU  6.28318530717958647692528676655900577
#define TAUf 6.28318530717958647692528676655900577f

#ifdef HLSL

#define FLOAT2 float2
#define FLOAT3 float3
#define FLOAT4 float4
#define VECTOR float4
#define MATRIX float4x4
#define UINT   uint

#else

#pragma once
#include <DirectXMath.h>

#define FLOAT2 DirectX::XMFLOAT2
#define FLOAT3 DirectX::XMFLOAT3
#define FLOAT4 DirectX::XMFLOAT4
#define VECTOR DirectX::XMVECTOR
#define MATRIX DirectX::XMMATRIX

// HOST-ONLY TYPEDEFS

typedef UINT16 Index;

#endif

// SHARED TYPEDEFS

struct Vertex {
    FLOAT3 position;
    FLOAT3 color;
};

struct RaytracingGlobals {
    MATRIX camera_to_world;
    float  camera_aspect;
    float  camera_focal_length;
};

#undef FLOAT2
#undef FLOAT3
#undef FLOAT4
#undef VECTOR
#undef MATRIX
