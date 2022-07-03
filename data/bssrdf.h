int   radii        = 2000;
int   radii_pitch  = 2048;
float radius_step  = 10e-9;

int   lambdas      = 63;
float lambda_0     = 380; // nm
float lambda_step  = 10;  // nm

alignas(max_align_t) const static float data[] = {
#include "data/bssrdf.dat"
};
