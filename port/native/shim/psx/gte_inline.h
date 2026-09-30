#ifndef PSX_GTE_INLINE_H
#define PSX_GTE_INLINE_H
/* Native shim for include/psx/gte_inline.h: the game-facing GTE macros, same names, implemented over the software GTE.
 * The original expands to MIPS COP2 assembly; this one calls into port/native/gte/gte.c. Put this directory before the
 * game's include directory (-I .../native/shim -I .../include). Only the macros game code actually uses are provided.
 * The linked-libgte macros (PSYQ_GTE_*) are not provided on purpose: libgte itself is replaced by libgte_native.c. */
#include "psx/gte_regs.h"
#include "gte.h"

typedef struct { gte_u32 w[3]; } gte_shim_vec3_t;

static inline void gte_shim_ldv0(const void* p) { gte_load_sv(0, p); }
static inline void gte_shim_ldv3(const void* a, const void* b, const void* c) {
    gte_load_sv(0, a); gte_load_sv(1, b); gte_load_sv(2, c);
}
static inline void gte_shim_stlvnl(void* p) {
    gte_u32* d = (gte_u32*)p;
    d[0] = gte_mfc2(GTE_D_MAC1); d[1] = gte_mfc2(GTE_D_MAC2); d[2] = gte_mfc2(GTE_D_MAC3);
}
static inline void gte_shim_stflg(void* p) { *(gte_u32*)p = gte_cfc2(GTE_C_FLAG); }
static inline void gte_shim_stsxy(void* p) { *(gte_u32*)p = gte_mfc2(GTE_D_SXY2); }
static inline void gte_shim_stsxy3(void* a, void* b, void* c) {
    *(gte_u32*)a = gte_mfc2(GTE_D_SXY0); *(gte_u32*)b = gte_mfc2(GTE_D_SXY1); *(gte_u32*)c = gte_mfc2(GTE_D_SXY2);
}
static inline void gte_shim_set_rot(const void* m) { int i; for (i = 0; i < 5; i++) gte_ctc2(GTE_C_R11R12 + i, ((const gte_u32*)m)[i]); }
static inline void gte_shim_set_trans(const void* m) { int i; for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, ((const gte_u32*)m)[5 + i]); }

#define gte_ldv0(r0)              gte_shim_ldv0(r0)
#define gte_ldv3(r0, r1, r2)      gte_shim_ldv3((r0), (r1), (r2))
#define gte_ldv3_split(r0, r1, r2) gte_shim_ldv3((r0), (r1), (r2))     /* battle_gfx_construct_polygon_data_for_units keeps its own asm copy */
#define gte_rtv0tr()              gte_command(0x4a480012u)              /* MVMVA sf=1 RT*V0+TR */
#define gte_rtps()                gte_command(0x4a180001u)
#define gte_rtpt()                gte_command(0x4a280030u)
#define gte_stlvnl(r0)            gte_shim_stlvnl(r0)
#define gte_stflg(r0)             gte_shim_stflg(r0)
#define gte_stsxy(r0)             gte_shim_stsxy(r0)
#define gte_stsxy3(r0, r1, r2)    gte_shim_stsxy3((r0), (r1), (r2))
#define gte_stsxy3_split(r0, r1, r2) gte_shim_stsxy3((r0), (r1), (r2))
#define gte_RotTrans(r1, r2, r3) \
    do { gte_ldv0(r1); gte_rtv0tr(); gte_stlvnl(r2); gte_stflg(r3); } while (0)
#define gte_RotTrans_split(r1, r2, r3) gte_RotTrans(r1, r2, r3)
#define gte_SetRotMatrix(r)       gte_shim_set_rot(r)
#define gte_SetTransMatrix(r)     gte_shim_set_trans(r)

#endif
