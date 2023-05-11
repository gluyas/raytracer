#pragma once
#include "prelude.h"
#include "array.h"
#include "raytracing.h"
#include "parse_obj.h"

struct MeshFilePaths {
	const char* obj_path;
	const char* mtl_path;
};

void mesh_load(
		ArrayView<MeshFilePaths> obj_files,
		Aabb& aabb,
		Material mat,
		Array<GeometryInstance>& geometries,
		Array<Array<Vertex>>& all_vertices, 
		Array<Array<Index>>& all_indices);