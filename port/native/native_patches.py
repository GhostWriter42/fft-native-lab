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
]