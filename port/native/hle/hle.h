#ifndef HLE_H
#define HLE_H
/* High-level emulation (HLE) of the PlayStation SDK's hardware layer -- the platform layer of the native port -- written ONCE and
 * used by two machines: the R3000 interpreter (which runs the original machine code) and the native build. Each machine owns an
 * hle_t (frame counter, registered callbacks, CD position, ...) and supplies a small platform vtable (RAM access, guest calls,
 * the disc, a frame-boundary hook), so the two run the same SDK behaviour on their own RAM and can be compared frame by frame.
 *
 * Freestanding C (no libc). */

typedef struct hle_platform {
    void* ctx;
    unsigned (*r8)(void* ctx, unsigned addr);                                   /* read a byte of RAM (PS1 address) */
    void (*w8)(void* ctx, unsigned addr, unsigned value);                       /* write a byte of RAM */
    void (*write_bytes)(void* ctx, unsigned addr, const unsigned char* src, unsigned n);   /* bulk write into RAM */
    unsigned (*call)(void* ctx, unsigned addr, unsigned a0, unsigned a1);       /* run a guest function (a callback), return $v0 */
    int (*read_sector)(void* ctx, unsigned lba, unsigned char* dst2048);        /* the 2048 data bytes of one CD sector; 0 = failed */
    void (*frame)(void* ctx);                                                   /* called after every VSync(0): a chance to yield to the comparison driver */
    void (*log)(void* ctx, const char* what, unsigned a0, unsigned a1, unsigned a2);   /* optional SDK call log */
    int interp_reexec;                                                          /* the machine cannot yield inside an HLE call (the interpreter): a multi-blank VSync is re-executed once per blank */
    int (*is_overlay)(void* ctx, unsigned lba);                                 /* optional: 1 if a read starting at this sector loads a CODE overlay (the driver syncs first) */
    unsigned stack_lo, stack_hi;                                                /* optional: a second address range that counts as "the caller's stack" (the native main stack) */
    int (*valid)(void* ctx, unsigned addr);                                     /* optional: 1 if the address can be read (default: the 2 MiB of RAM); guards the walk of GPU ordering tables */
} hle_platform_t;

/* A log of the calls a machine made into the HLE, in order (the platform-layer traffic: what a renderer / audio backend would be asked to do).
 * Arguments that are pointers to caller-local structures are logged by CONTENT (RECT, DISPENV, DRAWENV, CdlLOC ...) so that two machines with different
 * stacks can be compared; everything else by value. The lockstep driver clears it every frame and compares the two machines' logs. */
#define HLE_TRACE_MAX 8192
typedef struct hle_trace_entry { const char* name; unsigned a[5]; } hle_trace_entry_t;     /* a[4] = hash of the pointee of the first pointer argument (0 if none) */

/* BIOS events (OpenEvent / TestEvent / DeliverEvent ...) as the memory-card code polls them */
#define HLE_EVENTS 32
typedef struct hle_event { unsigned desc, spec, used, enabled, ready; } hle_event_t;

#define HLE_OTDUMPS 8
#define HLE_OTDUMP_WORDS 4096
typedef struct hle {
    hle_platform_t p;
    unsigned frame_counter;
    unsigned tick_count;                                                        /* hle_tick() calls */
    unsigned hblank_quarters;                                                   /* VSync(1): four queries per scanline since the last blank (see hle.c) */
    unsigned cb_vsync, cb_drawsync, cb_cd_ready, cb_cd_read, cb_spu_transfer;
    unsigned cd_lba;
    int cd_streaming;                                                           /* CdRead2 is running: CdReady/CdGetSector deliver sectors from cd_lba on */
    unsigned cd_reads, cd_sectors;
    unsigned calls;                                                             /* HLE calls seen */
    int stop;                                                                   /* set by the frame hook to stop the machine (interpreter) */
    /* Why the machine handed control to the comparison driver (p.frame was called): */
    int sync_reason;                                                            /* HLE_SYNC_VSYNC or HLE_SYNC_OVERLAY */
    unsigned sync_arg0, sync_arg1, sync_arg2;                                   /* overlay load: destination address, sector count, first sector */
    hle_trace_entry_t trace[HLE_TRACE_MAX];                                     /* this frame's HLE calls */
    unsigned trace_n, trace_lost;
    int no_trace;                                                               /* set by the driver around calls that must not be logged */
    unsigned vsync_left;                                                        /* blanks still owed by a VSync(n) call being re-executed (interp_reexec) */
    int reexec;                                                                 /* set: the driver must re-execute the HLE call instead of returning from it */                                               /* entries in use / calls that did not fit */
    hle_event_t ev[HLE_EVENTS];
    int in_cb;                                                                  /* inside a vertical-blank callback / event handler */
    unsigned rcnt2_handler, rcnt2_slot;                                         /* the sound driver's root-counter-2 event handler (called four times per vertical blank) and its event slot */
    unsigned pad_mask;                                                          /* controller 1 buttons as PadRead() returns them (set by the driver's input script) */
    unsigned bad_ot_addr, bad_ot_tag, bad_ot_head;                              /* first broken ordering-table chain seen: the tag word's address and value, and the table's head */
    /* what the frame's first DrawOTag calls drew, for diagnosing a mismatch of their hashes: per call the packets as {address, length, words...} */
    struct hle_otdump { unsigned trace_idx, n; unsigned w[HLE_OTDUMP_WORDS]; } otdump[HLE_OTDUMPS];
    unsigned otdump_n;
    struct spu* spu;                                                            /* optional software SPU (spu.h): when set, the libspu calls drive its voices (audio output) */
    struct gpu* gpu;                                                            /* optional software GPU (gpu.h): when set, the drawing calls render into its VRAM */
    int fast_effects;                                                           /* set by the driver while the BATTLE overlay is loaded and run.cfg "fasteffects 1": see VSync in hle.c */
    struct mcard* card;                                                         /* optional virtual memory card (card.h) in slot 0; without one, every card call reports no card */
} hle_t;
enum { HLE_SYNC_NONE = 0, HLE_SYNC_VSYNC = 1, HLE_SYNC_OVERLAY = 2, HLE_SYNC_DIVERGED = 3 };

void hle_init(hle_t* h, const hle_platform_t* p);
/* One tick of virtual time (a polling function was entered): every fourth tick is a vertical blank -- the game's own callback runs, no frame sync happens. */
void hle_tick(hle_t* h);
/* Perform the SDK function `name` (it takes `nargs` parameters: the guest's other argument registers hold garbage and are zeroed) with the guest's first
 * four arguments; returns $v0. Unknown names are logged and return 0. */
unsigned hle_call(hle_t* h, const char* name, unsigned nargs, unsigned a0, unsigned a1, unsigned a2, unsigned a3);

/* The names the HLE takes over (everything the game reaches in libspu / libetc / libcd / libcard, the BIOS event API and the
 * hardware-facing libgpu entry points). X(name) is expanded once per name. */
#define HLE_EXPLICIT_NAMES(X) \
    X(ResetGraph) X(SetGraphDebug) X(DrawSync) X(DrawSyncCallback) X(PutDispEnv) X(PutDrawEnv) X(DrawOTag) X(DrawPrim) \
    X(LoadImage) X(StoreImage) X(MoveImage) X(ClearImage) X(SetDispMask) X(GetGraphType) \
    X(OpenEvent) X(CloseEvent) X(EnableEvent) X(DisableEvent) X(TestEvent) X(WaitEvent) X(DeliverEvent) X(UnDeliverEvent) \
    X(EnterCriticalSection) X(ExitCriticalSection) X(SetMem) X(PadInit) X(PadRead) X(PadStop) X(_otc)

/* Routines the lockstep skips on BOTH machines (the interpreter never runs the original bytes, the native side gets a no-op). Hand-written render-only routines used
 * to be listed here until they had native versions (the text blitters: replacements/world_asm.c, battle_asm2.c; the four BATTLE map polygon queuers: battle_asm3.c,
 * generated from the machine code by tools/mips2c.py); what is left is the FMV start. */
#define HLE_SKIP_NAMES(X) \
    X(open_movie_start_stream)

/* SDK functions in the hooked libraries that are NOT taken over: pure RAM bookkeeping (no hardware access), so the real code runs on both machines. The SPU heap
 * (SpuInitMalloc / SpuMalloc / SpuFree on the game's own table) used to be a no-op that returned 0 for every allocation; the reverb getter reads the state
 * that hle.c keeps for the replaced setters (libspu_state()). */
#define HLE_KEEP_NAMES(X)     X(SpuInitMalloc) X(SpuMalloc) X(SpuFree) X(_spu_gcSPU) X(_SpuIsInAllocateArea) X(_SpuIsInAllocateArea_) X(SpuGetReverbModeParam)

/* Functions whose ENTRY lets virtual time pass (they poll a variable that the vertical-blank interrupt updates, in a loop with no SDK call): they are
 * not replaced, only observed -- every entry calls hle_tick() on both machines (see hle.c). */
#define HLE_TICK_NAMES(X) \
    X(wldcore_file_poll_vram_image_stream)

/* open_movie_start_stream is skipped as well: it drives the CD streaming / MDEC hardware (not modelled by the HLE yet). Without it the FMV counts as
 * instantly over (the movie-active flag it would set is never set), so the lockstep runs past the movies. */

#endif
