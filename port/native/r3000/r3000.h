#ifndef R3000_H
#define R3000_H
/* A small MIPS R3000A (PlayStation CPU) interpreter: the "oracle" for the native port.
 *
 * It runs the game's ORIGINAL machine code (SCUS_942.21 / overlay images loaded at their link addresses) so that a native
 * function can be compared, bit for bit, against the code the console ran. CPU only: no video, sound, CD or DMA -- the
 * hardware register window reads as zero and is counted so a test can notice when a function touches hardware.
 *
 * Implemented: the whole MIPS-I integer set (branch and load delay slots included, unaligned lwl/lwr/swl/swr), COP2 through
 * the software GTE in ../gte (register moves, lwc2/swc2, commands), the BIOS A-table entries the game's C code reaches
 * (rand, srand, abs, labs and a few memory/string helpers), syscall 1/2 (critical section) as no-ops. Not implemented: exceptions, interrupts, COP0
 * beyond harmless reads/writes, caches, cycle timing. An unsupported instruction stops execution and sets `fault`.
 *
 * Freestanding (no libc). */

typedef struct r3k {
    unsigned int r[32];
    unsigned int hi, lo;
    unsigned int pc, npc;               /* npc = address of the instruction after the one at pc (delay slot handling) */
    unsigned int ld_reg, ld_val;        /* pending load (visible after the next instruction) */
    unsigned char* ram;                 /* 2 MiB, mirrored through 8 MiB */
    unsigned char scratch[1024];        /* 0x1f800000 scratchpad */
    unsigned int bios_rand_seed;        /* BIOS rand()/srand() state */
    unsigned int sp_top;                /* initial stack pointer for r3k_call */
    unsigned long long steps;           /* instructions executed since reset */
    unsigned int io_reads, io_writes;   /* accesses outside RAM/scratchpad */
    unsigned int io_last_addr;
    unsigned int syscalls, bios_calls, rand_calls;
    int fault;                          /* 0 = ok; see R3K_FAULT_* */
    unsigned int fault_pc, fault_instr, fault_addr;
    unsigned int cur_pc;                /* address of the instruction being executed */
    unsigned int div_zero, div_overflow;/* divisions by zero / INT_MIN / -1 seen (defined on MIPS, traps on x86) */
    unsigned int watch_lo, watch_hi;    /* writes to [watch_lo, watch_hi) are logged (debugging aid; empty by default) */
    unsigned int wlog_n, wlog_pc[16], wlog_addr[16], wlog_val[16];
    unsigned int trace_calls, call_n, call_trace[1024];   /* targets of jal/jalr/j while trace_calls is set (debugging aid) */
    void (*call_hook)(struct r3k* c, unsigned int target);  /* optional: called when a traced jump target is REACHED (after its delay slot ran) */
    void (*event_hook)(struct r3k* c, unsigned int target); /* optional: the same for EVERY traced jump target (call_hook stops at the first 1024) */
    unsigned int pending_call;          /* jump target waiting for its delay slot to finish */
    unsigned int ncode, code_lo[4096], code_hi[4096];   /* optional sorted code ranges: data loads from them are counted in code_reads */
    unsigned int code_reads, code_writes;
    unsigned int wild;                  /* data accesses that a native build cannot reach: KUSEG/KSEG1 aliases, mirrors above 2 MiB, scratchpad */
    unsigned int nsdk, sdk_lo[16], sdk_hi[16], sdk_hits;   /* optional SDK ranges: calls/jumps into them are counted */
    /* HLE: code addresses (function entries) whose execution is intercepted -- hle(c, addr) runs INSTEAD of the function and its
     * return is then taken through $ra. Arguments are in a0-a3 / the stack, the result goes to v0. hle_bitmap: 1 bit per word of RAM. */
    int (*hle)(struct r3k* c, unsigned int addr);
    unsigned char hle_bitmap[0x200000 / 4 / 8];
    unsigned int hle_calls;
    /* zero_frames: every stack frame is zero-filled when it is allocated (`addiu sp,sp,-N`), so locals the code never initialised read as 0 -- the same rule the
     * native build gets from -ftrivial-auto-var-init=zero; the retail machine has whatever an earlier call left there, which the two builds cannot share. */
    unsigned int zero_frames;
} r3k_t;

enum { R3K_OK = 0, R3K_FAULT_BAD_FETCH = 1, R3K_FAULT_UNSUPPORTED = 2, R3K_FAULT_UNALIGNED = 3, R3K_FAULT_TIMEOUT = 4,
       R3K_FAULT_BIOS = 5, R3K_FAULT_BREAK = 6, R3K_FAULT_OVERFLOW = 7 };

#define R3K_SENTINEL 0xfffffff0u        /* return address planted in $ra by r3k_call */

void r3k_reset(r3k_t* c, unsigned char* ram);
void r3k_hle_add(r3k_t* c, unsigned int addr);                    /* intercept the function whose first instruction is at addr */
/* Call the function at `addr` with up to 8 integer arguments (a0-a3 then the stack, as the o32 ABI does).
 * Returns R3K_OK if it returned to the sentinel; the result is c->r[2] (and c->r[3]). */
int r3k_call(r3k_t* c, unsigned int addr, const unsigned int* args, int nargs, unsigned long long max_steps);
/* Run from the current pc until it reaches R3K_SENTINEL (the caller plants the return address). */
int r3k_run(r3k_t* c, unsigned long long max_steps);
/* Call guest code from inside an HLE handler: the register file, hi/lo, pc and load-delay state are saved and restored around the
 * call; the callee's stack frame is carved out below the current $sp. Returns the callee's $v0. */
unsigned int r3k_call_nested(r3k_t* c, unsigned int addr, const unsigned int* args, int nargs, unsigned long long max_steps);

unsigned int r3k_read32(r3k_t* c, unsigned int addr);
void r3k_write32(r3k_t* c, unsigned int addr, unsigned int value);

#endif
