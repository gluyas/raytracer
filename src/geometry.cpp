#include "geometry.h"

Triangle triangle_load_from_3_indices(ArrayView<Vertex> vertices, Index* indices) {
    Triangle t;
    t.a = XMLoadFloat3(&vertices[indices[0]].position);
    t.b = XMLoadFloat3(&vertices[indices[1]].position);
    t.c = XMLoadFloat3(&vertices[indices[2]].position);
    return t;
}

XMVECTOR triangle_cross(Triangle t) {
    XMVECTOR b = t.b - t.a;
    XMVECTOR c = t.c - t.a;
    return XMVector3Cross(b, c);
}

XMVECTOR triangle_normal(Triangle t) {
    return XMVector3Normalize(triangle_cross(t));
}

float triangle_area(Triangle t) {
    float parallelogram;
    XMStoreFloat(&parallelogram, XMVector3Length(triangle_cross(t)));
    return 0.5 * parallelogram;
}

XMVECTOR triangle_barycentric(Triangle t, float b, float c) {
    XMVECTOR point = t.a;
    point +=  (t.b - t.a) * b;
    point +=  (t.c - t.a) * c;
    return point;
}

Aabb aabb_join(Aabb a, Aabb b) {
    a.min = XMVectorMin(a.min, b.min);
    a.max = XMVectorMax(a.max, b.max);
    return a;
}

Aabb aabb_join(Aabb a, XMVECTOR v) {
    a.min = XMVectorMin(a.min, v);
    a.max = XMVectorMax(a.max, v);
    return a;
}

Aabb aabb_join(Aabb a, Triangle t) {
    a = aabb_join(a, t.a);
    a = aabb_join(a, t.b);
    a = aabb_join(a, t.c);
    return a;
}

XMVECTOR aabb_size(Aabb a) {
    return a.max - a.min;
}

float aabb_widest(Aabb a) {
    XMFLOAT3 size;
    XMStoreFloat3(&size, aabb_size(a));
    return fmax(fmax(size.x, size.y), size.z);
}
