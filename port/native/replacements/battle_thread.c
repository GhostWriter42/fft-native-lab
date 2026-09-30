/* Native cooperative threads for the BATTLE overlay.
 *
 * Retail: 16 thread records of 0x400 bytes in RAM (g_battle_threads); battle_thread_yield (handwritten, 0x8014ca80) saves
 * s0-s7/k0/k1/gp/sp/fp/ra into the current record, advances g_battle_current_thread_id to the next record whose is_running
 * word (+0x48) is set (wrapping to thread 0 unconditionally), calls battle_script_route_event_input for the new current
 * thread, restores that record's registers and returns into it. A fresh thread starts because battle_thread_start stores the
 * entry point in the record's saved-ra slot (code_pointer, +0x44) and a fresh stack pointer.
 *
 * Native: identical scheduling on the RAM records (so every other function that reads them sees the same values), but the
 * machine context (esp/ebp/ebx/esi/edi + a private native stack per slot) lives outside RAM in g_nt_ctx / g_nt_stacks, because
 * a 0x400-byte record cannot hold an x86 stack. Snapshots for rollback must therefore include these (see nt_* below).
 * Slot 0 is the main (game loop) context: it runs on the process stack.
 *
 * Replaces src/battle/battle_thread_start.c (it must also reset the slot's native context). */
#include "fft/battle.h"
#include "psx/types.h"

#define NT_SLOTS NATIVE_THREAD_SLOT_COUNT
#define NT_STACK_BYTES 0x40000

typedef struct nt_ctx {
    void* esp;          /* saved stack pointer while the slot is not running */
    s32 started;        /* 0: the next switch to this slot builds a fresh frame that enters the record's code_pointer */
} nt_ctx_t;

static nt_ctx_t g_nt_ctx[NT_SLOTS] = { { 0, 1 } };            /* slot 0 = the running main context */
static unsigned char g_nt_stacks[NT_SLOTS][NT_STACK_BYTES] __attribute__((aligned(16)));

/* void nt_switch(void** save_esp, void* new_esp): push the callee-saved registers, store esp, load the other esp, pop, return. */
void nt_switch(void** save_esp, void* new_esp);
__asm__(".text\n"
        ".globl nt_switch\n"
        "nt_switch:\n"
        "    movl 4(%esp), %eax\n"
        "    movl 8(%esp), %edx\n"
        "    pushl %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl %esp, (%eax)\n"
        "    movl %edx, %esp\n"
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n");

/* First code run on a fresh thread stack: the retail context "returns" into code_pointer with ra == code_pointer, so a thread
 * function that returns simply starts over; do the same. */
static void nt_thread_entry(void) {
    for (;;) {
        void (*entry)(void) = g_battle_threads[g_battle_current_thread_id].code_pointer;
        entry();
    }
}

static void nt_switch_to(s32 from, s32 to) {
    if (!g_nt_ctx[to].started) {
        u32* top = (u32*)(g_nt_stacks[to] + NT_STACK_BYTES);
        top -= 5;                                              /* padding keeps esp 16-byte aligned + 4 at nt_thread_entry */
        *--top = (u32)nt_thread_entry;                         /* nt_switch's `ret` */
        *--top = 0;                                            /* ebp */
        *--top = 0;                                            /* ebx */
        *--top = 0;                                            /* esi */
        *--top = 0;                                            /* edi */
        g_nt_ctx[to].esp = top;
        g_nt_ctx[to].started = 1;
    }
    nt_switch(&g_nt_ctx[from].esp, g_nt_ctx[to].esp);
}

void battle_thread_yield(void) {
    s32 current = g_battle_current_thread_id;
    s32 next = current;
    for (;;) {
        next += 1;
        g_battle_current_thread_id = next;
        if (next == NT_SLOTS) {                               /* wrapped: thread 0 runs whether or not its flag is set */
            next = 0;
            g_battle_current_thread_id = 0;
            break;
        }
        if (g_battle_threads[next].is_running != 0) {
            break;
        }
    }
    battle_script_route_event_input();                        /* retail calls it here, before the registers of the new thread are restored */
    if (next != current) {
        nt_switch_to(current, next);
    }
}

/* Same record writes as the retail C function (src/battle/battle_thread_start.c); additionally the slot's native context is reset. */
void battle_thread_start(s32 thread_id, void (*function)(void)) {
    native_thread_t* thread = &g_battle_threads[thread_id];
    thread->global_pointer = battle_thread_get_current_global_pointer();
    thread->stack_pointer = thread->stack_top;
    thread->frame_pointer = thread->stack_top;
    thread->code_pointer = function;
    thread->is_running = 1;
    thread->task_id = 0;
    thread->function_parameter_4 = 0;
    thread->task_words[0] = 0;
    thread->task_words[1] = 0;
    thread->task_words[2] = 0;
    thread->task_words[3] = 0;
    thread->task_words[4] = 0;
    thread->task_words[5] = 0;
    thread->task_words[6] = 0;
    if (thread_id != 0) {
        g_nt_ctx[thread_id].started = 0;
    }
}

/* Rollback support: the native half of the scheduler state. */
u32 nt_context_bytes(void) { return sizeof g_nt_ctx; }
