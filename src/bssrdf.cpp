#include "bssrdf.h"

namespace Bssrdf {

RawBssrdf get_default_bssrdf() {
    #include "data\bssrdf.h"

    RawBssrdf bssrdf = {};
    bssrdf.radii        = radii;
    bssrdf.radii_pitch  = radii_pitch;
    bssrdf.radius_step  = radius_step;

    bssrdf.lambdas      = lambdas;
    bssrdf.lambda_0     = lambda_0;
    bssrdf.lambda_step  = lambda_step;

    bssrdf.data         = data;

    return bssrdf;
}

Array<XMFLOAT3> generate_texture(RawBssrdf raw) {

}

} // namespace Bssrdf
