#include "bssrdf.h"

namespace Bssrdf {

SpectralBssrdf get_default_bssrdf() {
    #include "data\bssrdf.h"

    SpectralBssrdf bssrdf = {};
    bssrdf.radii        = radii;
    bssrdf.radii_pitch  = radii_pitch;
    bssrdf.radius_step  = radius_step;

    bssrdf.lambdas      = lambdas;
    bssrdf.lambda_0     = lambda_0;
    bssrdf.lambda_step  = lambda_step;

    bssrdf.data         = data;

    return bssrdf;
}

// piecewise gaussian
// rv = -0.5 / sigma^2
__m256 g(__m256 x, __m256 mu, __m256 rv1, __m256 rv2) {
    __m256 rv = _mm256_blendv_ps(rv1, rv2, _mm256_cmp_ps(x, mu, _CMP_GE_OS));

    __m256 d = _mm256_sub_ps(x, mu);
    return _mm256_exp_ps(_mm256_mul_ps(_mm256_mul_ps(d, d), rv));
}

Array<XMFLOAT3> convert_to_texture(SpectralBssrdf bssrdf) {
    UINT blocks_count = (bssrdf.lambdas - 1)/8 + 1;

    static Array<float>  weights             = {};
    static UINT          weights_lambdas     = 0;
    static float         weights_lambda_0    = 0;
    static float         weights_lambda_step = 0;
    if (weights_lambdas != bssrdf.lambdas || weights_lambda_0 != bssrdf.lambda_0 || weights_lambda_step != bssrdf.lambda_step) {
        // generate weights table
        weights.len = 0;
        array_reserve(&weights, 8 * 3*blocks_count);

        // naive conversion: spectral->XYZ; XYZ->RGB
        #define x8(x) x,x,x,x,x,x,x,x
        #define rv(sd) (-0.5/(sd*sd))
        // gaussian XYZ color matching functions:   scale      mu            sd1           sd2
        const static alignas(32) float xc1[] = { x8(1.056), x8(599.8), x8(rv(37.9)), x8(rv(31.0)) };
        const static alignas(32) float xc2[] = { x8(0.362), x8(442.0), x8(rv(16.0)), x8(rv(26.7)) };
        const static alignas(32) float xc3[] = { x8(-.065), x8(501.1), x8(rv(20.4)), x8(rv(26.2)) };
        const static alignas(32) float yc1[] = { x8(0.821), x8(568.8), x8(rv(46.9)), x8(rv(40.5)) };
        const static alignas(32) float yc2[] = { x8(0.286), x8(530.9), x8(rv(16.3)), x8(rv(31.1)) };
        const static alignas(32) float zc1[] = { x8(1.217), x8(437.0), x8(rv(11.8)), x8(rv(36.0)) };
        const static alignas(32) float zc2[] = { x8(0.681), x8(459.9), x8(rv(26.0)), x8(rv(13.8)) };
        #undef rv
        #undef x8

        __m256 lambda_step = _mm256_set1_ps(weights_lambda_step);
        __m256 lambda      = _mm256_fmadd_ps(lambda_step, _mm256_set_ps(0,1,2,3,4,5,6,7), _mm256_set1_ps(weights_lambda_0));
        lambda_step = _mm256_mul_ps(lambda_step, _mm256_set1_ps(8));
        for (int i = 0; i < blocks_count; i++) {
            if (i > 0) lambda = _mm256_add_ps(lambda, lambda_step);

            __m256 x;
            x = _mm256_mul_ps(  _mm256_load_ps(xc1+0), g(lambda, _mm256_load_ps(xc1+8), _mm256_load_ps(xc1+16), _mm256_load_ps(xc1+24)));
            x = _mm256_fmadd_ps(_mm256_load_ps(xc2+0), g(lambda, _mm256_load_ps(xc2+8), _mm256_load_ps(xc2+16), _mm256_load_ps(xc2+24)), x);
            x = _mm256_fmadd_ps(_mm256_load_ps(xc3+0), g(lambda, _mm256_load_ps(xc3+8), _mm256_load_ps(xc3+16), _mm256_load_ps(xc3+24)), x);
            _mm256_store_ps(array_push_uninitialized(&weights, 8).ptr, x);

            __m256 y;
            y = _mm256_mul_ps(  _mm256_load_ps(yc1+0), g(lambda, _mm256_load_ps(yc1+8), _mm256_load_ps(yc1+16), _mm256_load_ps(yc1+24)));
            y = _mm256_fmadd_ps(_mm256_load_ps(yc2+0), g(lambda, _mm256_load_ps(yc2+8), _mm256_load_ps(yc2+16), _mm256_load_ps(yc2+24)), y);
            _mm256_store_ps(array_push_uninitialized(&weights, 8).ptr, y);

            __m256 z;
            z = _mm256_mul_ps(  _mm256_load_ps(zc1+0), g(lambda, _mm256_load_ps(zc1+8), _mm256_load_ps(zc1+16), _mm256_load_ps(zc1+24)));
            z = _mm256_fmadd_ps(_mm256_load_ps(zc2+0), g(lambda, _mm256_load_ps(zc2+8), _mm256_load_ps(zc2+16), _mm256_load_ps(zc2+24)), z);
            _mm256_store_ps(array_push_uninitialized(&weights, 8).ptr, z);
        }

        weights_lambdas     = bssrdf.lambdas;
        weights_lambda_0    = bssrdf.lambda_0;
        weights_lambda_step = bssrdf.lambda_step;
    }

    Array<XMFLOAT3> texture = array_init<XMFLOAT3>(bssrdf.radii);
    for (UINT i = 0; i < bssrdf.radii; i++) {
        const float* row = bssrdf.data + i*bssrdf.radii_pitch;

        __m256 xs = _mm256_set1_ps(0);
        __m256 ys = xs;
        __m256 zs = xs;
        for (UINT j = 0; j < blocks_count; j++) {
            __m256 lambdas;
            if (j == blocks_count-1 && 8*blocks_count > bssrdf.lambdas) {
                __m256i indices = _mm256_add_epi32(_mm256_set1_epi32(8*j), _mm256_set_epi32(0,1,2,3,4,5,6,7));
                __m256i mask    = _mm256_cmpgt_epi32(_mm256_set1_epi32(bssrdf.lambdas), indices);
                lambdas = _mm256_maskload_ps(row + 8*j, mask);
            } else {
                lambdas = _mm256_load_ps(row + 8*j);
            }

            float* weight = &weights[8 * 3*j];
            xs = _mm256_fmadd_ps(lambdas, _mm256_load_ps(weight + 0),  xs);
            ys = _mm256_fmadd_ps(lambdas, _mm256_load_ps(weight + 8),  ys);
            zs = _mm256_fmadd_ps(lambdas, _mm256_load_ps(weight + 16), zs);
        }

        XMFLOAT3 xyz = {};
        // compute horizontal sums
        alignas(32) float unpack[24];
        _mm256_store_ps(unpack+0,  xs);
        _mm256_store_ps(unpack+8,  ys);
        _mm256_store_ps(unpack+16, zs);
        for (UINT i = 0; i < 8; i++) {
            xyz.x += unpack[0  + i];
            xyz.y += unpack[8  + i];
            xyz.z += unpack[16 + i];
        }
        array_push(&texture, xyz);
    }

    const static XMFLOAT4X4A xyz_to_rgb = {

    };
    XMMATRIX m =

    for (auto& xyz : texture) {
        XMVECTOR rgb = XMLoadFloat3(&xyz);
        rgb =
    }

}

} // namespace Bssrdf
