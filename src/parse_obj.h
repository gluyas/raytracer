#pragma once
#include "prelude.h"

void parse_obj_file(const char* filename, bool convert_to_rhs, Array<Vertex>* vertices, Array<Index>* indices, Aabb* aabb = NULL);
