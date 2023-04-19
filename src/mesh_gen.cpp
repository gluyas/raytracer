#include "mesh_gen.h"

void mesh_load(
		ArrayView<const char*> obj_files, 
		Aabb& aabb,
		Material mat,
		Array<GeometryInstance>& geometries,
		Array<Array<Vertex>>& all_vertices, 
		Array<Array<Index>>& all_indices
	) {
	Array<Vertex> vertices = {};
    Array<Index>  indices  = {};

    for(auto file : obj_files) {
    	parse_obj_file(file, true, &vertices, &indices, &aabb);
    }

 	GeometryInstance geometry = {};
 	geometry.material = mat;
 	geometry.vertices = vertices;
    geometry.indices  = indices;

    array_push(&all_vertices, vertices);
    array_push(&all_indices,  indices);
    array_push(&geometries, geometry);
}