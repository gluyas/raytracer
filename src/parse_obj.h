#pragma once
#include "prelude.h"
#include "raytracing.h"

void parse_mtl_file(const char* filename, Material* mat, wchar_t* dds_filepath);
void parse_obj_file(const char* filename, bool convert_to_rhs, Array<Vertex>* vertices, Array<Index>* indices, Aabb* aabb = NULL);