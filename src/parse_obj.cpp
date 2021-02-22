#include "parse_obj.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "types.h"
using namespace DirectX;

#include "array.h"

UINT32 parse_floats(char** char_buf, UINT32 max_count, float* out) {
    char* substr = *char_buf;
    int i = 0;
    while (i < max_count) {
        // skip leading whitespace
        while (isspace(substr[0])) {
            if (substr[0] == '\r' || substr[0] == '\n') {
                substr = NULL;
                goto end;
            }
            substr += 1;
        }

        // parse the float and append to array
        out[i] = atof(substr);
        i += 1;

        // advance past the float literal
        while (!isspace(substr[0])) substr += 1;
    }
end:
    *char_buf = substr;
    return i;
}

void parse_obj_file(const char* filename, Array<Vertex>* vertices, Array<Index>* indices, Aabb* aabb) {
    FILE* file = fopen(filename, "rb");

    const UINT64 char_buf_len = 512;
    char* char_buf = (char*) malloc(char_buf_len * sizeof(char));

    Array<XMFLOAT3> vs  = {};
    Array<XMFLOAT2> vts = {};
    Array<XMFLOAT3> vns = {};

    while (!feof(file)) {
        // read file
        fgets(char_buf, char_buf_len, file);
        int error = ferror(file);
        if (error) {
            fprintf(stderr, "error reading obj file");
            exit(error);
        }

        // parse
        char* substr = char_buf;
        if (substr[0] == 'v') {
            // vertex attribute
            if (isspace(substr[1])) {
                substr += 1;
                // position
                XMFLOAT3 v;
                assert(parse_floats(&substr, 3, (float*) &v) == 3);
                if (aabb) {
                    aabb->min.x = fmin(v.x, aabb->min.x);
                    aabb->max.x = fmax(v.x, aabb->max.x);
                    aabb->min.y = fmin(v.y, aabb->min.y);
                    aabb->max.y = fmax(v.y, aabb->max.y);
                    aabb->min.z = fmin(v.z, aabb->min.z);
                    aabb->max.z = fmax(v.z, aabb->max.z);
                }
                array_push(&vs, v);
            } else if (substr[1] == 't' && isspace(substr[2])) {
                substr += 2;
                // texture coordinate
                // NOTE: .obj spec supports 1-3 texture coordinates, this assumes 2
                XMFLOAT2 vt;
                assert(parse_floats(&substr, 2, (float*) &vt) == 2);
                array_push(&vts, vt);
            } else if (substr[1] == 'n' && isspace(substr[2])) {
                substr += 2;
                // normal
                XMFLOAT3 vn;
                assert(parse_floats(&substr, 3, (float*) &vn) == 3);
                array_push(&vns, vn);
            }
        } else if (substr[0] == 'f') {
            substr += 1;

            UINT32 v_indices [4]; UINT32 v_indices_count  = 0;
            UINT32 vt_indices[4]; UINT32 vt_indices_count = 0;
            UINT32 vn_indices[4]; UINT32 vn_indices_count = 0;

            int verts_count = 0;
            while (verts_count < 4) {
                // skip leading whitespace
                while (isspace(substr[0])) {
                    if (substr[0] == '\r' || substr[0] == '\n') {
                        substr = NULL;
                        goto end_face;
                    }
                    substr += 1;
                }
                verts_count += 1;

                // always expect a vertex point index
                v_indices[v_indices_count] = atoi(substr) - 1;
                v_indices_count += 1;

                while (substr[0] != '/') {
                    substr += 1;
                    if (isspace(substr[0])) goto end_vertex;
                }
                substr += 1;
                if (substr[0] != '/') {
                    // take texture coordinate after the first slash
                    vt_indices[vt_indices_count] = atoi(substr) - 1;
                    vt_indices_count += 1;

                    while (substr[0] != '/') {
                        substr += 1;
                        if (isspace(substr[0])) goto end_vertex;
                    }
                }
                substr += 1;

                // take vertex normal after second slash
                vn_indices[vn_indices_count] = atoi(substr) - 1;
                vn_indices_count += 1;

            end_vertex:
                // advance past this vertex
                while (!isspace(substr[0])) substr += 1;
            }
        end_face:
            assert(verts_count >= 3);
            assert(v_indices_count  == verts_count);
            assert(vt_indices_count == verts_count || vt_indices_count == 0);
            assert(vn_indices_count == verts_count || vn_indices_count == 0);

            // create the triangle indices
            Index base_index = vertices->len;
            if (verts_count == 3) {
                Index face_indices[] = {
                    (Index) (base_index+0), (Index) (base_index+1), (Index) (base_index+2)
                };
                array_concat(indices, VLA_VIEW(face_indices));
            } else if (verts_count == 4) {
                Index face_indices[] = {
                    (Index) (base_index+0), (Index) (base_index+1), (Index) (base_index+2),
                    (Index) (base_index+0), (Index) (base_index+2), (Index) (base_index+3)
                };
                array_concat(indices, VLA_VIEW(face_indices));
            } else {
                assert(false);
            }

            // create the vertices from the indexed data
            // TODO: deduplicate
            for (int i = 0; i < verts_count; i++) {
                Vertex vertex = {};
                vertex.position = vs[v_indices[i]];

                if (vn_indices_count) vertex.normal = vns[vn_indices[i]];
                else {
                    // create rudimentary face normals if vertex normals were not specified
                    // TODO: generate weighted vertex normals
                    XMVECTOR a = XMLoadFloat3(&vs[v_indices[0]]);
                    XMVECTOR b = XMLoadFloat3(&vs[v_indices[1]]) - a;
                    XMVECTOR c = XMLoadFloat3(&vs[v_indices[2]]) - a;
                    XMStoreFloat3(&vertex.normal, XMVector3Normalize(XMVector3Cross(b, c)));
                }
                // if (vt_indices_count); // TODO: handle uv coordinates
                array_push(vertices, vertex);
            }
        }
    }
    free(char_buf);
    fclose(file);
}
