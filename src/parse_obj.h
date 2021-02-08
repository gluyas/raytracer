#pragma once

#include "types.h"
#include "array.h"

void parse_obj_file(const char* filename, Array<Vertex>* vertices, Array<Index>* indices);
