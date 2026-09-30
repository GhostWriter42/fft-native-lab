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
        file='src/battle/battle_unit_start_post_attack_animation_display.c',
        old='((void (*)(void))battle_unit_set_target_animation_from_attack_type)();',
        new='battle_unit_set_target_animation_from_attack_type((battle_unit_misc_data_t*)flag, (battle_unit_misc_data_t*)ctx);',
        why='the retail call loads no arguments, so the callee receives this function\'s own $a0/$a1 (flag, ctx); replay of the original on '
            'the interpreter shows args (flag, ctx) at the callee\'s entry. (Function fuzz: native crashed dereferencing stale stack.)',
    ),
    dict(
        file='src/main/main_party_save_unit.c',
        old='((void (*)(void))main_party_remove_unit)();',
        new='main_party_remove_unit(index);',
        why='the retail call loads no argument, so main_party_remove_unit receives the $a0 that still holds `index` (lbu a0,2(s2) at the top of '
            'main_party_save_unit; disassembly at 0x80059c0c). Function fuzz: native passed a stale value.',
    ),
    dict(
        file='src/battle/battle_gfx_set_thrown_item_graphic_palette.c',
        old='g_main_item_item_flags = get_item_data_pointer();',
        new='g_main_item_item_flags = (u8*)main_item_get_data_pointer(graphic_id);',
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
        file='src/battle/battle_map_step_init_sequence.c',
        old='        ((void (*)(void))main_sound_stop_sfx)();\n        step++;\n        battle_camera_init_defaults();',
        new='        main_sound_stop_sfx(map_id);\n        step++;\n        battle_camera_init_defaults();',
        why='same as battle_map_init_units_sprites_event_and_music.c step 0: $a0 is still the function\'s first parameter (map_id) at 0x8008eaa4.',
    ),
]