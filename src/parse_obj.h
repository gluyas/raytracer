#pragma once
#include "prelude.h"

void parse_obj_file(const char* filename, bool swap_yz, Array<Vertex>* vertices, Array<Index>* indices, Aabb* aabb = NULL);
