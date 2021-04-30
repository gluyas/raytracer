#pragma once
#include "prelude.h"

struct Triangle {
    XMVECTOR a;
    XMVECTOR b;
    XMVECTOR c;
};

Triangle load_triangle_from_3_indices(ArrayView<Vertex> vertices, Index* indices) {
    Triangle t;
    t.a = XMLoadFloat3(&vertices[indices[0]].position);
    t.b = XMLoadFloat3(&vertices[indices[1]].position);
    t.c = XMLoadFloat3(&vertices[indices[2]].position);
    return t;
}

float triangle_area(Triangle t) {
    XMVECTOR a = 0.5 * XMVector3Length(triangle_cross(t));

    float area;
    XMStoreFloat(&area, t.a);
    return area;
}

XMVECTOR triangle_normal(Triangle t) {
    return XMVector3Normalize(triangle_cross(t));
}

XMVECTOR triangle_cross(Triangle t) {
    XMVECTOR b = t.b - t.a;
    XMVECTOR c = t.c - t.a;
    return XMVector3Cross(b, c);
}

XMVECTOR triangle_barycentric(Triangle t, float b, float c) {
    XMVECTOR point = t.a;
    point +=  (t.b - t.a) * b;
    point +=  (t.c - t.a) * c;
    return point;
}
