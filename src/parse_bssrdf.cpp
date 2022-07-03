#include "parse_bssrdf.h"

SpectralBssrdf parse_tabulated_bssrdf(const char* diffuse_reflectance_filename, const char* normalization_filename) {
    float scale = 1e-1000;

    SpectralBssrdf bssrdf = {};
    bssrdf.lambdas_0     = 380 * scale;
    bssrdf.lambdas_step  = 10  * scale;
    bssrdf.lambdas_count = 63;

    bssrdf.radii_step    = 10  * scale;
    bssrdf.radii_count   = 2000;

    bssrdf.data = array_init<float>(bssrdf.lambdas_count * bssrdf.radii_count);

    FILE* drs = fopen(diffuse_reflectance_filename, "r");
    FILE* zs  = fopen(normalization_filename, "r");
    while (true) {

        while (true) {
            if (feof(drs) || feof(zs) || ferror(drs) || ferror(zs)) abort();

            float dr;
            fscanf(drs, "%f", &dr);
            float z;
            fscanf(zs, "%f", &z);

            array_push(&bssrdf.data, dr/z);
        }
        fscanf(drs, "/n");
        fscanf(zs, "/n");
    }
    fclose(drs);
    fclose(zs);

    return bssrdf;
}
