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

Array<XMFLOAT3> convert_to_texture(SpectralBssrdf bssrdf) {
    Array<XMFLOAT3> texture = array_init<XMFLOAT3>(bssrdf.radii);

    UINT blocks_count = (bssrdf.lambdas - 1)/8 + 1;

    static Array<__m256> weights             = {};
    static UINT          weights_lambdas     = 0;
    static float         weights_lambda_0    = 0;
    static float         weights_lambda_step = 0;
    if (weights_lambdas != bssrdf.lambdas || weights_lambda_0 != bssrdf.lambda_0 || weights_lambda_step != bssrdf.lambda_step) {
        // generate weights table
        weights.len = 0;
        array_reserve(&weights, blocks_count);

        // naive conversion: spectral->XYZ->RGB
        __m256 lambda_step = _mm256_set1_ps(weights_lambda_step);
        __m256 lambda      = _mm256_fmadd_ps(lambda_step, _mm256_set_ps(0,1,2,3,4,5,6,7), _mm256_set1_ps(weights_lambda_0));
        lambda = _mm256_mul_ps(lambda, _mm256_set1_ps(8));

        for (int i = 0; i < blocks_count; i++) {
            if (i > 0) lambda = _mm256_add_ps(lambda, lambda_step);

            // evaluate g function
            __m256 x;
            __m256 mu;
            __m256 s1;
            __m256 s2;
            __m256 sigma = _mm256_cmp_ps(
        }

        weights_lambdas     = bssrdf.lambdas;
        weights_lambda_0    = bssrdf.lambda_0;
        weights_lambda_step = bssrdf.lambda_step;
    }



}

} // namespace Bssrdf
