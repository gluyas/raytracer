#pragma once
#include "prelude.h"

struct RawBssrdf {
    // row: radius
    UINT  radii;
    UINT  radii_pitch;
    float radius_step;  // m

    // column: wavelength (lambda)
    UINT  lambdas;
    float lambda_0;     // nm
    float lambda_step;  // nm

    // data: index with [radius_index*radii_pitch + lambda_index]
    const float* data;
};

namespace Bssrdf {

RawBssrdf get_default_bssrdf();
Array<XMFLOAT3> generate_texture(RawBssrdf raw);

} // namespace Bssrdf
