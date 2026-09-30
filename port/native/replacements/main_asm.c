/* Native C versions of the routines of the main executable that the retail binary keeps as raw bytes or that manipulate the MIPS stack
 * pointer (the C compiler cannot reproduce them); see battle_asm.c for the BATTLE overlay's.
 *
 * Replaces src/main/main.c (it must also capture the native context of the soft-reset point) and
 * src/main/main_system_store_stack_pointer.c (listed in replaced.txt). */
#include "fft/main.h"
#include "psx/types.h"

/* 0x800449ec: `nop; jr ra; nop` -- a callable no-op (main_file_load_to_address calls it in its wait loops). */
void main_noop_800449ec(void) {}

/* Retail: main() stores its $sp in g_main_system_game_loop_stack_pointer; a soft reset (main_system_reset_game) calls
 * main_restore_game_loop_stack_pointer, which puts that $sp back and jumps to main_system_run_game_loop again, abandoning every frame
 * above main(). Natively the abandoned stack is unwound with a builtin setjmp/longjmp pair; the RAM word keeps the value retail
 * stores (main runs with $sp = 0x801fffe0 there) so that the RAM image stays identical, but nothing reads it. */
static void* g_game_loop_context[5];

void main_system_store_stack_pointer(u32* destination) { *destination = 0x801fffe0u; }

void main(void) {
    main_boot_run_startup();
    main_system_store_stack_pointer(&g_main_system_game_loop_stack_pointer);
    (void)__builtin_setjmp(g_game_loop_context);                  /* returns again (1) when a soft reset unwinds to here */
    main_system_run_game_loop();
    PadStop();
    StopCallback();
}

void main_restore_game_loop_stack_pointer(u32* saved_stack_pointer) { __builtin_longjmp(g_game_loop_context, 1); }
