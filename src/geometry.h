#pragma once
#include "prelude.h"

struct Triangle {
    XMVECTOR a;
    XMVECTOR b;
    XMVECTOR c;
};

Triangle triangle_load_from_3_indices(ArrayView<Vertex> vertices, Index* indices);
XMVECTOR triangle_cross(Triangle t);
XMVECTOR triangle_normal(Triangle t);
float    triangle_area(Triangle t);
XMVECTOR triangle_barycentric(Triangle t, float b, float c);

struct Aabb {
    XMVECTOR min;
    XMVECTOR max;
};

#define AABB_NULL (Aabb { g_XMInfinity, g_XMNegInfinity })

Aabb aabb_join(Aabb a, XMVECTOR v);
Aabb aabb_join(Aabb a, Aabb b);
Aabb aabb_join(Aabb a, Triangle t);

XMVECTOR aabb_size(Aabb a);
float    aabb_widest(Aabb a);
