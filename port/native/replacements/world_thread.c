/* Native cooperative threads for the WORLD overlay: the twin of battle_thread.c (read its header comment for the design).
 *
 * Retail: 17 thread records of 0x400 bytes in RAM (g_world_threads -> g_world_thread_contexts); world_thread_yield (handwritten, 0x800fff50)
 * saves s0-s7/k0/k1/gp/sp/fp/ra into the current record, advances g_world_thread_current_id to the next record whose is_running word
 * (+0x48) is set (wrapping to thread 0 unconditionally), calls world_script_update_event_input_state for the new current thread, restores
 * that record's registers and returns into it. world_thread_start stores the entry point in the record's saved-ra slot (code_pointer, +0x44)
 * and a fresh stack pointer.
 *
 * Native: identical scheduling on the RAM records, machine context (esp/ebp/ebx/esi/edi + a private stack per slot) outside RAM.
 *
 * Replaces src/world/world_thread_start.c (it must also reset the slot's native context). */
#include "fft/world.h"
#include "psx/types.h"
extern unsigned g_ls_ignore_enter;

#define WT_SLOTS 17

typedef struct wt_ctx {
    void* esp;          /* saved stack pointer while the slot is not running */
    s32 started;        /* 0: the next switch to this slot builds a fresh frame that enters the record's code_pointer */
} wt_ctx_t;

static wt_ctx_t g_wt_ctx[WT_SLOTS] = { { 0, 1 } };            /* slot 0 = the running main context */
#ifdef LOCKSTEP_THREAD_WINDOW                                 /* whole-program lockstep: the stacks sit in the mapped window above RAM (thread_window.h) */
#include "thread_window.h"
#define WT_STACK_BYTES THREAD_STACK_BYTES
#define WT_STACK(slot) ((unsigned char*)(THREAD_STACK_WINDOW + THREAD_STACK_WORLD_OFFSET + (unsigned)(slot) * THREAD_STACK_BYTES))
#else
#define WT_STACK_BYTES 0x40000
static unsigned char g_wt_stacks[WT_SLOTS][WT_STACK_BYTES] __attribute__((aligned(16)));
#define WT_STACK(slot) (g_wt_stacks[slot])
#endif

void wt_switch(void** save_esp, void* new_esp);
__asm__(".text\n"
        ".globl wt_switch\n"
        "wt_switch:\n"
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

static void wt_thread_entry(void) {
    for (;;) {
        void (*entry)(void) = g_world_threads[g_world_thread_current_id].code_pointer;
        g_ls_ignore_enter = 1;                                 /* (lockstep replay) retail enters a thread by returning into it: no call event */
        entry();
    }
}

static void wt_switch_to(s32 from, s32 to) {
    if (!g_wt_ctx[to].started) {
        u32* top = (u32*)(WT_STACK(to) + WT_STACK_BYTES);
        top -= 5;
        *--top = (u32)wt_thread_entry;
        *--top = 0;                                            /* ebp */
        *--top = 0;                                            /* ebx */
        *--top = 0;                                            /* esi */
        *--top = 0;                                            /* edi */
        g_wt_ctx[to].esp = top;
        g_wt_ctx[to].started = 1;
    }
    wt_switch(&g_wt_ctx[from].esp, g_wt_ctx[to].esp);
}

void world_thread_yield(void) {
    s32 current = g_world_thread_current_id;
    s32 next = current;
    for (;;) {
        next += 1;
        g_world_thread_current_id = next;
        if (next == WT_SLOTS) {                               /* wrapped: thread 0 runs whether or not its flag is set */
            next = 0;
            g_world_thread_current_id = 0;
            break;
        }
        if (g_world_threads[next].is_running != 0) {
            break;
        }
    }
    world_script_update_event_input_state();                  /* retail calls it here, before the registers of the new thread are restored */
    if (next != current) {
        wt_switch_to(current, next);
    }
}

void world_thread_start(s32 thread_id, void (*function)(void)) {
    native_thread_t* thread = &g_world_threads[thread_id];
    thread->global_pointer = world_thread_get_current_global_pointer();
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
    if (thread_id > 0 && thread_id < WT_SLOTS) {               /* retail writes the record for any id; only real slots have a native context */
        g_wt_ctx[thread_id].started = 0;
    }
}

u32 wt_context_bytes(void) { return sizeof g_wt_ctx; }
