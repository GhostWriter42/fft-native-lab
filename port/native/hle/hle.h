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
    int (*is_overlay)(void* ctx, unsigned lba);                                 /* optional: 1 if a read starting at this sector loads a CODE overlay (the driver syncs first) */
} hle_platform_t;

/* A log of the calls a machine made into the HLE, in order (the platform-layer traffic: what a renderer / audio backend would be asked to do).
 * Arguments that are pointers to caller-local structures are logged by CONTENT (RECT, DISPENV, DRAWENV, CdlLOC ...) so that two machines with different
 * stacks can be compared; everything else by value. The lockstep driver clears it every frame and compares the two machines' logs. */
#define HLE_TRACE_MAX 8192
typedef struct hle_trace_entry { const char* name; unsigned a[5]; } hle_trace_entry_t;     /* a[4] = hash of the pointee of the first pointer argument (0 if none) */

typedef struct hle {
    hle_platform_t p;
    unsigned frame_counter;
    unsigned cb_vsync, cb_drawsync, cb_cd_ready, cb_cd_read, cb_spu_transfer;
    unsigned cd_lba;
    unsigned cd_reads, cd_sectors;
    unsigned calls;                                                             /* HLE calls seen */
    int stop;                                                                   /* set by the frame hook to stop the machine (interpreter) */
    /* Why the machine handed control to the comparison driver (p.frame was called): */
    int sync_reason;                                                            /* HLE_SYNC_VSYNC or HLE_SYNC_OVERLAY */
    unsigned sync_arg0, sync_arg1, sync_arg2;                                   /* overlay load: destination address, sector count, first sector */
    hle_trace_entry_t trace[HLE_TRACE_MAX];                                     /* this frame's HLE calls */
    unsigned trace_n, trace_lost;
    int no_trace;                                                               /* set by the driver around calls that must not be logged */                                               /* entries in use / calls that did not fit */
    unsigned pad_mask;                                                          /* controller 1 buttons as PadRead() returns them (set by the driver's input script) */
} hle_t;
enum { HLE_SYNC_NONE = 0, HLE_SYNC_VSYNC = 1, HLE_SYNC_OVERLAY = 2 };

void hle_init(hle_t* h, const hle_platform_t* p);
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

/* Hand-written RENDER-ONLY routines of the overlays (they only build pixel images and GPU packets that the renderer consumes). The lockstep
 * skips them on BOTH machines (the interpreter never runs the original bytes, the native side gets a no-op), so the rest of the game can be
 * compared without transliterating them first; their native versions are a separate verification job (harness_diff_asm.c). */
#define HLE_SKIP_NAMES(X) \
    X(battle_map_queue_textured_triangles) X(battle_map_queue_textured_quads) X(battle_map_queue_untextured_triangles) X(battle_map_queue_untextured_quads) \
    X(blit_text_glyph) X(battle_text_render_glyph_to_4bpp_image) X(world_text_blit_glyph) X(world_text_blit_font_glyph_to_4bpp)

#endif
