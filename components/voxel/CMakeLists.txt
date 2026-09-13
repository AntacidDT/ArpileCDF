idf_component_register(SRCS
    "arpile_app_voxel.c"
    "voxel_math.c"
    "voxel_camera.c"
    "voxel_texture.c"
    "voxel_renderer.c"
    "voxel_block.c"
    "voxel_chunk.c"
    "voxel_mesh.c"
    "voxel_generator.c"
    "voxel_world.c"
    "voxel_game.c"
    "voxel_player.c"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    REQUIRES arpile_ui esp_mm heap)