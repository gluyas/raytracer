#include "mesh_gen.h"

void mesh_load(
		ArrayView<MeshFilePaths> obj_files,
		Aabb& aabb,
		Material mat,
		Array<GeometryInstance>& geometries,
		Array<Array<Vertex>>& all_vertices, 
		Array<Array<Index>>& all_indices
	) {
	Array<Vertex> vertices = {};
    Array<Index>  indices  = {};
    wchar_t* dds_filepath = nullptr;

    for(auto file : obj_files) {
    	parse_obj_file(file.obj_path, true, &vertices, &indices, &aabb);
        //for now assume if mtl exists there's only one obj being loaded
        if (strlen(file.mtl_path) > 1){
            dds_filepath = (wchar_t*)malloc(256*sizeof(char));
            parse_mtl_file(file.mtl_path, &mat, dds_filepath);
        }
    }

 	GeometryInstance geometry = {};
 	geometry.material = mat;
 	geometry.vertices = vertices;
    geometry.indices  = indices;
    geometry.dds_filepath = dds_filepath;

    
    //load texture file if present
    /*if (dds_filepath != nullptr) {
        CHECK_RESULT(LoadDDSTextureFromFile(Device::g_device, dds_filepath, A ptr to resource, ddsData, C));
        free(dds_filepath);
    }*/

    array_push(&all_vertices, vertices);
    array_push(&all_indices,  indices);
    array_push(&geometries, geometry);
}