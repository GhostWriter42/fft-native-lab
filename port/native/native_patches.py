"""Reviewed native-port source patches.

tools/portify.py applies these to the PORTIFIED copies of the sources (never to the repository): exact-match text replacements at the
few places where the retail code depends on a MIPS ABI accident that a native compiler does not reproduce. Every entry must match its
file exactly once, otherwise portify stops -- so a change upstream that touches a patched line is noticed instead of silently ignored.

Kinds of site (the oracle -- native vs the original machine code -- found each of them; see NATIVE-RUNTIME.md section 7):
  * a function is called without its arguments and reads the caller's leftover $a0..$a3 (`((void (*)(void))f)()` casts): the patch passes
    the values the registers hold at that point;
  * a `void` function whose last computation is left in $v0 and consumed by the caller: the patch makes the value explicit.

Each patch documents WHICH register value the retail code passes and how that was established (replay of the original on the interpreter).
"""

PATCHES = [
    dict(
        file='src/world/world_card_build_save_slot_description.c',
        old='((u8 * (*)(u32)) world_gfx_bind_data_pointer)(1)',
        new='(world_gfx_bind_data_pointer(1), g_world_text_job_names)',
        why='world_gfx_bind_data_pointer(1) stores g_world_text_job_names_data into g_world_text_job_names and leaves that value in $v0; the '
            'caller uses it as the job-name text table. (Function fuzz: native crashed reading a stale eax.)',
    ),
    dict(
        file='src/battle/battle_gfx_set_thrown_item_graphic_palette.c',
        old='g_main_item_item_flags = get_item_data_pointer();',
        new='g_main_item_item_flags = (u8*)main_item_get_data_pointer(graphic_id);',
        alt=('item_data = get_item_data_pointer();', 'item_data = (u8*)main_item_get_data_pointer(graphic_id);'),     # the semantic-cleanup spelling
        why='get_item_data_pointer is main_item_get_data_pointer(item_id) (same address); the retail call loads no argument, so it receives this '
            'function\'s first parameter, which is still in $a0.',
    ),
    dict(
        file='src/battle/battle_effect_try_init_data.c',
        old='((s32 (*)(void))battle_effect_init_data)()',
        new='battle_effect_init_data(0)',
        why='battle_effect_init_data(result) returns its own argument (the caller\'s stale $a0) only when g_effect_load_state is outside 0..3, which '
            'the game never produces; every reachable state assigns the result. 0 = "no work remains" is the deterministic native choice.',
    ),
    dict(
        file='src/battle/battle_state_enter_effect_playback.c',
        old='((s32 (*)(void))battle_effect_init_data)()',
        new='battle_effect_init_data(0)',
        why='see battle_effect_try_init_data.c',
    ),
    dict(
        file='src/battle/battle_state_handle_start_effect_file_open_state.c',
        old='((s32 (*)(void))battle_effect_init_data)()',
        new='battle_effect_init_data(0)',
        why='see battle_effect_try_init_data.c',
    ),
    dict(
        file='src/battle/battle_unit_advance_altima_teleport_distortion.c',
        old='((s32 (*)(void))battle_effect_init_data)()',
        new='battle_effect_init_data(0)',
        count=2,
        why='see battle_effect_try_init_data.c',
    ),
    dict(
        file='src/battle/battle_map_init_units_sprites_event_and_music.c',
        old='        ((void (*)(void))main_sound_stop_sfx)();\n        battle_camera_init_defaults();',
        new='        main_sound_stop_sfx(map_id);\n        battle_camera_init_defaults();',
        why='step 0 calls main_sound_stop_sfx without loading its sound id, so it receives the function\'s own $a0 = map_id (still untouched: the '
            'prologue only copies it to s1). Lockstep replay of the original at the first battle: main_sound_stop_sfx a0=0x3e = the map id; the '
            'native call passed a stale stack word, matched an active SFX channel and released its voices (control flow diverged). The other '
            'call site (step 11) leaves $a0 as whatever main_sound_unload_current_scenario_music ended with -- still unpatched.',
    ),
    dict(
        file='src/battle/battle_unit_clear_status_staging_data.c',
        old='void battle_unit_clear_status_staging_data(void) {\n    s32 i;\n',
        new='void battle_unit_clear_status_staging_data(void) {\n    s32 i;\n    if (g_battle_unit_status_staging_data == 0) {\n        return;\n    }\n',
        why='the first battle event runs this before battle_unit_update_staged_status_data has ever set the staging pointer (lockstep: '
            'g_battle_unit_status_staging_data is still 0 at frame 1600, set only later), so retail stores zeros through a NULL base: into the '
            'console\'s low RAM (KUSEG 0x39c..), i.e. the BIOS kernel area, which nothing here reads. Natively that address is unmapped '
            '(SIGSEGV); the guard drops the store. (RAM outside the kernel area is unaffected.)',
    ),
    dict(
        file='src/world/world_unit_clear_status_staging_data.c',
        old='    s32 unit_index;\n\n    unit_index = 0;\n    do {\n        g_world_unit_status_staging_data->state[unit_index] = 0;',
        new='    s32 unit_index;\n\n    if (g_world_unit_status_staging_data == 0) {\n        return;\n    }\n    unit_index = 0;\n    do {\n        g_world_unit_status_staging_data->state[unit_index] = 0;',
        why='twin of battle_unit_clear_status_staging_data (same NULL-base store into the kernel area before the first update).',
    ),
    dict(
        file='src/battle/battle_target_calculate_tile_coords_with_cursor_glow.c',
        old='    SVECTOR corner0;\n    SVECTOR corner1;\n    SVECTOR corner2;\n    SVECTOR corner3;\n',
        new='    SVECTOR corner_array[4];\n',
        why='battle_target_calculate_cursor_tile_polygon(coords, layer, SVECTOR* quad) fills quad[0..3] and is called with &corner0: it relies on the '
            'four SVECTOR locals being adjacent in the frame (MIPS gcc 2.6.3 lays them out that way; a native compiler does not, so the writes to '
            'quad[1..3] landed elsewhere and corner1..3 stayed empty). Lockstep replay, frame 8273 of a random-play run: battle_gfx_build_cursor_tile_glow '
            'a3 (= &corner1) pointed at zeros natively, at {0xc4, -132, ..} in the original. The four vertices become one array (this and the two edits below).',
    ),
    dict(
        file='src/battle/battle_target_calculate_tile_coords_with_cursor_glow.c',
        old='g_battle_cursor_z, &corner0);',
        new='g_battle_cursor_z, &corner_array[0]);',
        why='see above',
    ),
    dict(
        file='src/battle/battle_target_calculate_tile_coords_with_cursor_glow.c',
        old='&corner0, &corner1, &corner2, &corner3, main_gfx_get_otag() + depth);',
        new='&corner_array[0], &corner_array[1], &corner_array[2], &corner_array[3], main_gfx_get_otag() + depth);',
        why='see above',
    ),
    # --- the event/status-banner "thread status indicator" drawers (six twins) read the banner thread's parameter block through a POINTER that is NULL when the
    # --- banner thread was started without one: retail then reads console RAM at 0..0xb (the BIOS area: zero in the oracle). Natively address 0 is unmapped.
    dict(
        files=['src/event/attack_menu_draw_thread_status_indicators.c', 'src/event/bunit_menu_draw_thread_status_indicators.c',
               'src/event/debugchr_menu_draw_thread_status_indicators.c', 'src/event/require_menu_draw_thread_status_indicators.c'],
        old='    native_thread_t* thread;\n    native_thread_t* descriptor;\n',
        new='    native_thread_t* thread;\n    native_thread_t* descriptor;\n    static native_thread_t g_low_memory_zero_thread; /* what a NULL parameter block reads as (low RAM: zero) */\n',
        why='lockstep soak seeds 10 and 23: NATIVE CRASH reading through NULL in bunit_menu_draw_thread_status_indicators. g_battle_threads[8].function_parameter_1 is a pointer '
            'to the banner thread\'s parameter block; when it is 0 the retail code reads console RAM at 0x0/0x4/0x8 (BIOS area, zero: `function_parameter_1 == 0`, `_2 == 0`, '
            '`_3 & 0x80 == 0` -- a panel is requested) and draws normally. A zero stand-in object reproduces those reads.',
    ),
    dict(
        files=['src/event/attack_menu_draw_thread_status_indicators.c', 'src/event/bunit_menu_draw_thread_status_indicators.c',
               'src/event/debugchr_menu_draw_thread_status_indicators.c', 'src/event/require_menu_draw_thread_status_indicators.c'],
        old='            thread = (native_thread_t*)g_battle_threads[8].function_parameter_1;\n',
        new='            thread = (native_thread_t*)g_battle_threads[8].function_parameter_1;\n            if (thread == 0) {\n                thread = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        files=['src/event/attack_menu_draw_thread_status_indicators.c', 'src/event/bunit_menu_draw_thread_status_indicators.c',
               'src/event/debugchr_menu_draw_thread_status_indicators.c', 'src/event/require_menu_draw_thread_status_indicators.c'],
        old='            descriptor = (native_thread_t*)g_battle_threads[7].function_parameter_1;\n',
        new='            descriptor = (native_thread_t*)g_battle_threads[7].function_parameter_1;\n            if (descriptor == 0) {\n                descriptor = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        file='src/event/equip_menu_draw_thread_status_indicators.c',
        old='    native_thread_t* thread;\n    native_thread_t* descriptor;\n',
        new='    native_thread_t* thread;\n    native_thread_t* descriptor;\n    static native_thread_t g_low_memory_zero_thread; /* what a NULL parameter block reads as (low RAM: zero) */\n',
        why='EQUIP twin of the banner-thread NULL parameter block (threads 13 and 14); see attack_menu_draw_thread_status_indicators.c.',
    ),
    dict(
        file='src/event/equip_menu_draw_thread_status_indicators.c',
        old='            thread = (native_thread_t*)g_battle_threads[13].function_parameter_1;\n',
        new='            thread = (native_thread_t*)g_battle_threads[13].function_parameter_1;\n            if (thread == 0) {\n                thread = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        file='src/event/equip_menu_draw_thread_status_indicators.c',
        old='            descriptor = (native_thread_t*)g_battle_threads[14].function_parameter_1;\n',
        new='            descriptor = (native_thread_t*)g_battle_threads[14].function_parameter_1;\n            if (descriptor == 0) {\n                descriptor = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        file='src/world/world_menu_draw_thread_status_indicators.c',
        old='    native_thread_t* thread;\n    native_thread_t* descriptor;\n',
        new='    native_thread_t* thread;\n    native_thread_t* descriptor;\n    static native_thread_t g_low_memory_zero_thread; /* what a NULL parameter block reads as (low RAM: zero) */\n',
        why='WORLD twin of the banner-thread NULL parameter block; see attack_menu_draw_thread_status_indicators.c.',
    ),
    dict(
        file='src/world/world_menu_draw_thread_status_indicators.c',
        old='            thread = (native_thread_t*)g_world_threads[8].function_parameter_1;\n',
        new='            thread = (native_thread_t*)g_world_threads[8].function_parameter_1;\n            if (thread == 0) {\n                thread = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        file='src/world/world_menu_draw_thread_status_indicators.c',
        old='            descriptor = (native_thread_t*)g_world_threads[7].function_parameter_1;\n',
        new='            descriptor = (native_thread_t*)g_world_threads[7].function_parameter_1;\n            if (descriptor == 0) {\n                descriptor = &g_low_memory_zero_thread;\n            }\n',
        why='see above.',
    ),
    dict(
        file='src/event/bunit_cmd_run_stream.c',
        old='data = ((u8 * (*)(void)) g_bunit_cmd_handlers[data[0]])();',
        new='data = g_bunit_cmd_handlers[data[0]](data);',
        why='the decomp calls the opcode handler through a `(void)` cast, but the handlers take the stream pointer in $a0 (their table type is u8* (*)(u8*)). '
            'Disassembly of bunit_cmd_run_stream (0x801c8564): a0 = data at the first `jalr v0` and `move a0,v0` after every call, so each handler receives the '
            'current stream pointer. Natively the handler read a stale stack word (soak seeds 10, 23, 37: jump to a garbage address in the unit menu).',
    ),
    dict(
        file='src/event/equip_cmd_run_stream.c',
        old='stream = ((u8 * (*)(void)) g_equip_cmd_handlers[stream[0]])();',
        new='stream = g_equip_cmd_handlers[stream[0]](stream);',
        why='twin of bunit_cmd_run_stream.c (the EQUIP overlay\'s command-stream runner, handler type u8* (*)(u8*), argument left in $a0 by the retail code).',
    ),
    dict(
        file='src/battle/battle_menu_run_scrolling_ability_list_thread.c',
        old='        SetSemiTrans(&frame, 1);\n',
        new='        /* retail: SetSemiTrans(&frame, 1) -- the address of the local POINTER: it sets bit 1 of the byte at &frame + 7, which in the MIPS frame\n'
            '         * (sp+0xc3) is padding after `toggle`. A native frame has a live local there. No effect in retail, so no call here. */\n',
        why='lockstep soak (seeds 4, 12, 17, 26, 30, 32): the scroll-list thread drew different arrows / crashed natively. The call passes &frame (a stack slot '
            'holding the SPRT pointer) instead of frame; SetSemiTrans sets `code |= 2` at offset 7 of its argument, i.e. one byte past the slot. Disassembly '
            '(0x8013a144: addiu a0,sp,188; sw s3,188(sp)): the neighbouring word at sp+192 is `toggle` (s16, sp+192..193), the byte at sp+195 is padding, so the '
            'retail write is harmless; the native compiler puts another local there and the write corrupted it. Dropping the call is exactly equivalent.',
    ),
    dict(
        file='src/event/helpmenu_run_battle_help_menu.c',
        old='    POLY_FT4 cursor_polys[2];\n    POLY_FT4 shadow_polys[2];\n',
        new='    POLY_FT4 poly_pool[4];\n#define cursor_polys poly_pool\n#define shadow_polys (poly_pool + 2)\n',
        why='the function addresses its primitives as `horiz_poly[2]` (cursor_polys[2], i.e. shadow_polys[0]): it relies on the two 2-element arrays being '
            'adjacent in the frame, as MIPS gcc 2.6.3 lays them out. A native compiler places them elsewhere, so the shadow quads kept their initial off-screen '
            'vertices ({-512, 0}) and the writes through horiz_poly[2..] landed in other locals. Found by lockstep soak (seed 7, frame 11377: DrawOTag packet '
            '#604 native {-512,0} x4 vs original {214,10}...). The two arrays become one pool of four.',
    ),
    dict(
        file='src/event/helpmenu_menu_run_require_help.c',
        old='    POLY_FT4 cursor_polys[2];\n    POLY_FT4 shadow_polys[2];\n',
        new='    POLY_FT4 poly_pool[4];\n#define cursor_polys poly_pool\n#define shadow_polys (poly_pool + 2)\n',
        why='twin of helpmenu_run_battle_help_menu.c (same adjacent-arrays assumption).',
    ),
    dict(
        file='src/battle/battle_map_step_init_sequence.c',
        old='        ((void (*)(void))main_sound_stop_sfx)();\n        step++;\n        battle_camera_init_defaults();',
        new='        main_sound_stop_sfx(map_id);\n        step++;\n        battle_camera_init_defaults();',
        why='same as battle_map_init_units_sprites_event_and_music.c step 0: $a0 is still the function\'s first parameter (map_id) at 0x8008eaa4.',
    ),
    dict(
        file='src/wldcore/wldcore_window_build_render_record_image.c',
        old='    *(wldcore_window_render_bounds16_t*)&g_wldcore_window_render_records[index].x\n        = *(wldcore_window_render_bounds16_t*)&position;\n',
        new='    /* retail: one 8-byte copy out of the a1/a2 argument save slots (position then dimensions, adjacent in the MIPS frame) */\n    ((wldcore_window_render_bounds16_t*)&g_wldcore_window_render_records[index].x)->position = position;\n    ((wldcore_window_render_bounds16_t*)&g_wldcore_window_render_records[index].x)->dimensions = dimensions;\n',
        why='the function copies the record bounds with `*(bounds16_t*)&position`, reading 8 bytes: the `position` argument and whatever lies after it. On MIPS the '
            'a1/a2 save slots are adjacent, so that is (position, dimensions); a native frame keeps them apart (lockstep, world map frame 3839: record w/h native '
            '0x000000b1 vs original 0x00300048, then a different LoadImage rectangle and window image). The patch copies the two arguments explicitly.',
    ),
    dict(
        file='src/world/world_menu_scrolling_list_thread.c',
        old='        SetSemiTrans(&frame, 1);\n',
        new='        /* retail: SetSemiTrans(&frame, 1) -- the address of the local POINTER (see battle_menu_run_scrolling_ability_list_thread.c): it sets bit 1 of the\n'
            '         * byte at &frame + 7, which in the MIPS frame (sp+0xc3) is padding after `toggle`. A native frame has a live local there. No effect in retail. */\n',
        why='lockstep world-map soak (seeds 103, 107, 108, 110, 112, 117, 119, 120, 124): twin of the BATTLE scroll-list thread. SetSemiTrans(&frame, 1) writes `code |= 2` at offset 7 '
            'of the address of the `frame` pointer; natively that byte is the top byte of the next local (row_offset became 0x02000000 and the list code crashed reading '
            '0x8416e4fe). Disassembly (0x800edfb0: addiu a0,sp,188; sw s2,188(sp)): `toggle` is the s16 at sp+192 and the next live word is sp+196, so sp+195 is padding '
            'and the retail write is harmless; dropping the call is exactly equivalent.',
    ),
    dict(
        file='src/battle/battle_ai_choose_wait_facing.c',
        old='    do {\n        work.target_coords.bytes.x = ai->acting_unit_coords.bytes.x + g_battle_ai_facing_tile_offsets.bytes[offset];',
        new='    work.past_viable_directions = 0;\n    do {\n        work.target_coords.bytes.x = ai->acting_unit_coords.bytes.x + g_battle_ai_facing_tile_offsets.bytes[offset];',
        done='u8 past_viable_directions;',                                      # newer upstream declares and handles the byte itself
        why='see the first entry: the past-the-end byte of the viability array is zero.',
    ),
    # --- NULL-pointer reads of console low RAM (a build that cannot map page zero, i.e. the Windows port, needs these; the oracle's low RAM reads as zero)
    dict(
        file='src/battle/battle_menu_build_unit_portrait_poly.c',
        old='    if (battle_unit_get_stats_from_battle_id(battle_id)->unit_id == g_main_special_portrait_unit_id) {',
        new='    if (({ battle_stats_t* stats_ = battle_unit_get_stats_from_battle_id(battle_id); stats_ ? stats_->unit_id : 0; }) == g_main_special_portrait_unit_id) {',
        why='battle_unit_get_stats_from_battle_id returns NULL for an id >= 21 (every lockstep seed reaches it: the NULL-page report lists battle_menu_build_unit_portrait_poly+46); retail reads '
            'unit_id (+0x161) from console low RAM, which is zero in the oracle. Explicit zero.',
    ),
    dict(
        file='src/battle/battle_unit_init_misc_data.c',
        old='        unit->battle_data->equipment[UNIT_EQUIPMENT_SLOT_RIGHT_HAND_WEAPON], unit);\n    battle_status_init_special_flag_enabling(unit->battle_data->misc_unit_id);',
        new='        unit->battle_data ? unit->battle_data->equipment[UNIT_EQUIPMENT_SLOT_RIGHT_HAND_WEAPON] : 0, unit);\n    battle_status_init_special_flag_enabling(unit->battle_data ? unit->battle_data->misc_unit_id : 0);',
        why='battle_unit_init_misc_data is called with stats == NULL for some units (it guards stats != 0 at the palette step); the end of the function then reads equipment[] and misc_unit_id '
            'through the NULL battle_data (NULL-page report: battle_unit_init_misc_data+1441). Retail reads zeros from low RAM; explicit zeros.',
    ),
    # --- stack buffer overflows that retail's frame layout absorbs
    dict(
        file='src/event/equip_menu_load_images_and_reset_lists.c',
        old='    u16 buf1[4];\n    u16 buf2[12];\n',
        new='    u16 pal[16];            /* retail: buf1[4] directly followed by buf2[12] in the frame (the 16-halfword store below fills both) */\n'
            '    u16* buf1 = pal;\n    u16* buf2 = pal + 4;\n',
        why='lockstep long soak (seeds 1109, 1116, 1121): equip_gfx_store_image_and_wait(&rect, buf1) stores a 16x1 VRAM rectangle (16 halfwords = 32 bytes) into the 4-halfword buf1. '
            'In retail buf2 sits right above buf1, so the store fills buf1+buf2 exactly (and the following buf2[0] = 0 clears word 4 of the 32 bytes that are then loaded back); a native frame '
            'puts the saved registers there: the palette data landed in the saved ebx and equip_menu_init_screen stored it as g_equip_unit_status_panel_flags (0x4a303527 instead of the unit id). '
            'One 32-byte array with buf2 at offset 4 halfwords reproduces the retail layout.',
    ),
    dict(
        file='src/event/equip_entrypoint.c',
        old='    /* The target loads no argument for this one-argument callee: $a0 still\n     * holds stats from the lookup above. */\n    ((void (*)(void))main_unit_refresh_stats_and_statuses)();',
        new='    main_unit_refresh_stats_and_statuses(stats);',
        why='lockstep long soak (seeds 1109, 1116, 1121: g_battle_unit_misc_data+0x148 native 8 vs 0, NULL-page reads in main_unit_update_stats_statuses_and_equipment): when the equipment '
            'screen closes, the decomp calls main_unit_refresh_stats_and_statuses with no argument. Retail leaves `stats` in $a0 (disassembly 0x801bf9a8: move a0,v0 ... 0x801bf9f4: jal '
            'main_unit_refresh_stats_and_statuses with a0 untouched), so it refreshes the edited unit; natively the callee got a stale stack word (NULL or another pointer) and '
            'refreshed the wrong memory. Passing `stats`.',
    ),
    dict(
        file='src/event/equip_menu_update_vertical_selection_and_mark_change.c',
        old='    /* The target leaves a2 (input_mask) unset for this three-argument callee. */\n    selection = ((s16 (*)(s32, s32))equip_menu_update_wrapped_vertical_selection)(entry_count & 0xFFFF, index);',
        new='    /* retail leaves a2 alone, so the wrapped-selection routine receives this function\'s own input_mask (disassembly 0x801c970c: no write to a2 before the jal) */\n    selection = equip_menu_update_wrapped_vertical_selection(entry_count & 0xFFFF, index, input_mask);',
        why='upstream (2b09e33) gave the wrapper its real parameters but still casts the inner call to two arguments. The callee reads its input mask from $a2, which the retail wrapper never touches: '
            'it is the wrapper\'s own third parameter. Natively the mask was stack garbage: seed 1116 of the long soak queued sound 6 and left the equipment screen on the wrong path at frame 8692. '
            '(Replaces the older patch of the same name for the pre-2b09e33 source text.)',
    ),
    dict(
        file='src/event/equip_menu_update_horizontal_selection_and_mark_change.c',
        old='    /* The target leaves a2 (input_mask) unset for this three-argument callee. */\n    selection = ((s16 (*)(s32, s32))equip_menu_update_wrapped_horizontal_selection)(entry_count & 0xFFFF, index);',
        new='    /* retail leaves a2 alone, so the wrapped-selection routine receives this function\'s own input_mask (disassembly 0x801c9818: no write to a2 before the jal) */\n    selection = equip_menu_update_wrapped_horizontal_selection(entry_count & 0xFFFF, index, input_mask);',
        why='twin of equip_menu_update_vertical_selection_and_mark_change.c.',
    ),
]
