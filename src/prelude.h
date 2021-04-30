// prelude for all project sources including HLSL shaders

#pragma once

// C++ INCLUDES
#ifdef CPP

// sytem libraries

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#ifdef DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif
#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

#include <DirectXMath.h>
using namespace DirectX;

// third-party libraries

#include "d3dx12.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "stb_image_write.h"

// utility headers

#include "array.h"

#endif

// COMMON TYPES

#ifdef CPP

#define COMMON_FLOAT    float
#define COMMON_FLOAT2   XMFLOAT2
#define COMMON_FLOAT3   XMFLOAT3
#define COMMON_FLOAT4   XMFLOAT4
#define COMMON_FLOAT4X4 XMFLOAT4X4A
#define COMMON_UINT     UINT32
#define COMMON_UINT2    XMUINT2
#define COMMON_UINT3    XMUINT3
#define COMMON_UINT4    XMUINT4

#endif
#ifdef HLSL

#define INFINITY (1.0 / 0.0)

#define COMMON_FLOAT    float
#define COMMON_FLOAT2   float2
#define COMMON_FLOAT3   float3
#define COMMON_FLOAT4   float4
#define COMMON_FLOAT4X4 float4x4
#define COMMON_UINT     uint
#define COMMON_UINT2    uint2
#define COMMON_UINT3    uint3
#define COMMON_UINT4    uint4

#endif

// shared typedefs

struct Aabb {
    COMMON_FLOAT3 min;
    COMMON_FLOAT3 max;
};

struct Vertex {
    COMMON_FLOAT3 position;
    COMMON_FLOAT3 normal;
};

struct SamplePoint {
    COMMON_FLOAT3 position;
    COMMON_FLOAT3 flux;
};

struct RaytracingGlobals {
    COMMON_UINT     frame_rng;
    COMMON_UINT     accumulator_count;
    COMMON_UINT     samples_per_pixel;
    COMMON_UINT     bounces_per_sample;

    COMMON_FLOAT4X4 camera_to_world;
    COMMON_FLOAT    camera_aspect;
    COMMON_FLOAT    camera_focal_length;

    COMMON_UINT     translucent_samples_count;
    COMMON_FLOAT    translucent_scattering;
    COMMON_FLOAT    translucent_absorption;
    COMMON_FLOAT    translucent_refraction;
    COMMON_UINT2    fpp;
};

struct RaytracingLocals {
    COMMON_UINT   primitive_index_offset;
    COMMON_FLOAT3 color;
};

// HELPER FUNCTIONS

// 16-bit index buffer support
#ifdef CPP
typedef UINT16 Index;
#endif
#ifdef HLSL
inline uint3 load_3x16_bit_indices(ByteAddressBuffer index_buffer, uint primitive_index) {
    const uint indices_per_primitive = 3;
    const uint bytes_per_index       = 2;
    const uint align_to_4bytes_mask  = ~0x0003;

    uint primitive_byte_index = primitive_index * indices_per_primitive * bytes_per_index;
    uint aligned_byte_index   = primitive_byte_index & align_to_4bytes_mask;

    dword2 four_indices = index_buffer.Load2(aligned_byte_index);

    uint3 indices;
    if (primitive_byte_index == aligned_byte_index) {
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
#endif

// winapi/d3d12 helpers
// TODO: nicer error handling
#define CHECK_RESULT(hresult) if ((hresult) != S_OK) abort()
#define SET_NAME(object) CHECK_RESULT(object->SetName(L#object))

// HELPFUL CONSTANTS

#define TAU  6.28318530717958647692528676655900577
#define TAUf 6.28318530717958647692528676655900577f
#define DEGREES (TAU / 360)

#ifdef HLSL
#define INFINITY (1.0 / 0.0)
#define NAN      (0.0 / 0.0)
#endif
