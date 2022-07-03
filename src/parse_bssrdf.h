#pragma once
#include "prelude.h"

struct SpectralBssrdf {
    Array<float> data;

    float lambdas_0;
    float lambdas_step;
    UINT  lambdas_count;

    float radii_step;
    UINT  radii_count;
};

SpectralBssrdf parse_tabulated_bssrdf(const char* diffuse_reflectance_filename, const char* normalization_filename);
