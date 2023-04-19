#pragma once
#include "prelude.h"
#include "array.h"
#include "raytracing.h"
#include "parse_obj.h"

void mesh_load(
		ArrayView<const char*> obj_files,
		Aabb& aabb,
		Material mat,
		Array<GeometryInstance>& geometries,
		Array<Array<Vertex>>& all_vertices, 
		Array<Array<Index>>& all_indices);