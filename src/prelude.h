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

// temporary vector/matrix typenames for portable struct definitions
#ifdef CPP

#define FLOAT2 XMFLOAT2
#define FLOAT3 XMFLOAT3
#define FLOAT4 XMFLOAT4
#define VECTOR XMVECTOR
#define MATRIX XMMATRIX

// host-only typedefs
typedef UINT16 Index;

#endif
#ifdef HLSL

#define INFINITY (1.0 / 0.0)

#define FLOAT2 float2
#define FLOAT3 float3
#define FLOAT4 float4
#define VECTOR float4
#define MATRIX float4x4
#define UINT32 uint

#endif

// shared typedefs

struct Aabb {
    FLOAT3 min;
    FLOAT3 max;
};

struct Vertex {
    FLOAT3 position;
    FLOAT3 normal;
};

struct SamplePoint {
    FLOAT3 position;
    FLOAT3 flux;
};

struct RaytracingGlobals {
    UINT32 frame_rng;
    UINT32 accumulator_count;
    MATRIX camera_to_world;
    float  camera_aspect;
    float  camera_focal_length;
    UINT32 samples_per_pixel;
    UINT32 bounces_per_sample;

    UINT32 translucent_samples_count;
    float  translucent_scattering;
    float  translucent_absorption;
    float  translucent_refraction;
};

struct RaytracingLocals {
    UINT32 primitive_index_offset;
    FLOAT3 color;
};

// HELPFUL CONSTANTS

#define TAU  6.28318530717958647692528676655900577
#define TAUf 6.28318530717958647692528676655900577f
#define DEGREES (TAU / 360)

#ifdef HLSL
#define INFINITY (1.0 / 0.0)
#define NAN      (0.0 / 0.0)
#endif

// clean up temporary typedefs
#undef FLOAT2
#undef FLOAT3
#undef FLOAT4
#undef VECTOR
#undef MATRIX
