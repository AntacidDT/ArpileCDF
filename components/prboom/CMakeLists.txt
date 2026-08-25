idf_component_register(SRCS
    "am_map.c" "d_client.c" "d_deh.c" "d_items.c" "d_main.c" "doomdef.c" "doomstat.c" "dstrings.c"
    "f_finale.c" "f_wipe.c" "g_game.c" "hu_lib.c" "hu_stuff.c" "info.c"
    "lprintf.c" "m_argv.c" "m_bbox.c" "m_cheat.c" "md5.c" "m_menu.c"
    "m_misc.c" "mmus2mid.c" "m_random.c"
    "p_ceilng.c" "p_doors.c" "p_enemy.c" "p_floor.c" "p_genlin.c"
    "p_inter.c" "p_lights.c" "p_map.c" "p_maputl.c" "p_mobj.c" "p_plats.c"
    "p_checksum.c" "p_pspr.c" "p_saveg.c" "p_setup.c" "p_sight.c" "p_spec.c" "p_switch.c"
    "p_telept.c" "p_tick.c" "p_user.c"
    "r_bsp.c" "r_data.c" "r_demo.c" "r_draw.c" "r_filter.c" "r_fps.c" "r_main.c"
    "r_patch.c" "r_plane.c" "r_segs.c" "r_sky.c" "r_things.c"
    "s_sound.c" "sounds.c" "st_lib.c" "st_stuff.c" "tables.c" "version.c"
    "v_video.c" "wi_stuff.c" "w_wad.c" "w_mmap.c" "z_bmalloc.c" "z_zone.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "prboom_psram.lf"
    REQUIRES prboom_wad_tables)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11 -Os
    -Wno-error=char-subscripts -Wno-error=unused-value -Wno-error=parentheses
    -Wno-error=int-to-pointer-cast -Wno-pointer-sign
    -Wno-error=unused-but-set-parameter -Wno-error=maybe-uninitialized
    -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types
    -Wno-error=type-limits -Wno-error=implicit-fallthrough
    -Wno-error=format-overflow -Wno-error=misleading-indentation
    -Wno-error=missing-field-initializers -Wno-error=duplicate-decl-specifier
    -Wno-error=address -Wno-error=dangling-else -Wno-error=sizeof-pointer-div -Wno-error=nonnull
    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare)
