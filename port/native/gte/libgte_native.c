/* Native replacement for the reconstructed Psy-Q libgte (the 30-odd functions the game calls), built on the software GTE.
 *
 * Each function is ported from the algorithm in its reconstruction under src/psyq/libgte/ (the reconstructions are written
 * as GTE/MIPS instruction sequences and cannot compile natively). Plain-C members of the library -- rsin, sin_1, rcos,
 * ratan2, csqrt, psyq_gte_csqrt_kernel -- are compiled from the repository unchanged and only need Lzc() and the
 * sine/atan tables from the RAM image.
 *
 * Tables (g_psyq_gte_*) are the game's own data, linked at their PS1 addresses through the RAM image. */
#include "psx/libgte.h"
#include "gte.h"

extern u32 g_psyq_gte_sin_cos_table[];
extern s16 g_psyq_gte_sqrt_table[];
extern s16 g_psyq_gte_inv_sqrt_table[];
extern s32 g_psyq_gte_matrix_stack_offset;
extern MATRIX g_psyq_gte_matrix_stack[20];

#define GTE_CMD_RTV0TR   0x4a480012u   /* MVMVA sf=1 RT * V0 + TR      */
#define GTE_CMD_RTPS     0x4a180001u
#define GTE_CMD_RTPT     0x4a280030u
#define GTE_CMD_NCLIP    0x4b400006u
#define GTE_CMD_MUL_COL  0x4a486012u   /* MVMVA sf=1 RT * V0, no translation */
#define GTE_CMD_LV_HIGH  0x4a41e012u   /* MVMVA sf=0 RT * IR, no translation */
#define GTE_CMD_LV_LOW   0x4a49e012u   /* MVMVA sf=1 RT * IR, no translation */
#define GTE_CMD_SQR0     0x4aa00428u   /* SQR sf=0 lm=1 */
#define GTE_CMD_GPF12    0x4b90003du   /* GPF sf=1 */

static u32 word(const void* p, int i) { return ((const u32*)p)[i]; }
static void load_rot(const void* m) { int i; for (i = 0; i < 5; i++) gte_ctc2(GTE_C_R11R12 + i, word(m, i)); }

/* --------------------------------------------------------------------------------- geometry set-up */
void SetGeomOffset(int x, int y) { gte_ctc2(GTE_C_OFX, (u32)(x << 16)); gte_ctc2(GTE_C_OFY, (u32)(y << 16)); }
void SetGeomScreen(int h) { gte_ctc2(GTE_C_H, (u32)h); }
s32 ReadGeomScreen(void) { return (s32)gte_cfc2(GTE_C_H); }
void InitGeom(void) {
    gte_ctc2(GTE_C_ZSF3, 341); gte_ctc2(GTE_C_ZSF4, 256); gte_ctc2(GTE_C_H, 1000);
    gte_ctc2(GTE_C_DQA, (u32)-4194); gte_ctc2(GTE_C_DQB, 0x01400000u);
    gte_ctc2(GTE_C_OFX, 0); gte_ctc2(GTE_C_OFY, 0);
}
void SetRotMatrix(MATRIX* m) { load_rot(m); }
void SetTransMatrix(MATRIX* m) { gte_ctc2(GTE_C_TRX, (u32)m->t[0]); gte_ctc2(GTE_C_TRY, (u32)m->t[1]); gte_ctc2(GTE_C_TRZ, (u32)m->t[2]); }
void SetColorMatrix(MATRIX* m) { int i; for (i = 0; i < 5; i++) gte_ctc2(GTE_C_LR1LR2 + i, word(m, i)); }
void SetLightMatrix(MATRIX* m) { int i; for (i = 0; i < 5; i++) gte_ctc2(GTE_C_L11L12 + i, word(m, i)); }
void SetBackColor(s32 r, s32 g, s32 b) { gte_ctc2(GTE_C_RBK, (u32)(r << 4)); gte_ctc2(GTE_C_GBK, (u32)(g << 4)); gte_ctc2(GTE_C_BBK, (u32)(b << 4)); }
void SetFarColor(s32 r, s32 g, s32 b) { gte_ctc2(GTE_C_RFC, (u32)(r << 4)); gte_ctc2(GTE_C_GFC, (u32)(g << 4)); gte_ctc2(GTE_C_BFC, (u32)(b << 4)); }
s32 Lzc(s32 value) { gte_mtc2(GTE_D_LZCS, (u32)value); return (s32)gte_mfc2(GTE_D_LZCR); }

/* ------------------------------------------------------------------------ vector transforms / projection */
long RotTrans(SVECTOR* in, VECTOR* out, long* flag) {
    long f;
    gte_load_sv(0, in);
    gte_command(GTE_CMD_RTV0TR);
    out->vx = (s32)gte_mfc2(GTE_D_MAC1); out->vy = (s32)gte_mfc2(GTE_D_MAC2); out->vz = (s32)gte_mfc2(GTE_D_MAC3);
    f = (long)gte_cfc2(GTE_C_FLAG);
    *flag = f;
    return f;
}
void RotTransSV(SVECTOR* in, SVECTOR* out, s32* flag) {
    gte_load_sv(0, in);
    gte_command(GTE_CMD_RTV0TR);
    out->vx = (s16)gte_mfc2(GTE_D_IR1); out->vy = (s16)gte_mfc2(GTE_D_IR2);
    ((s32*)out)[1] = (s32)gte_mfc2(GTE_D_IR3);                /* full-word IR3 store, as the hardware sequence does */
    *flag = (s32)gte_cfc2(GTE_C_FLAG);
}
s32 RotTransPers(SVECTOR* v0, s32* sxy, s32* p, s32* flag) {
    gte_load_sv(0, v0);
    gte_command(GTE_CMD_RTPS);
    *sxy = (s32)gte_mfc2(GTE_D_SXY2); *p = (s32)gte_mfc2(GTE_D_IR0); *flag = (s32)gte_cfc2(GTE_C_FLAG);
    return (s32)gte_mfc2(GTE_D_SZ3) >> 2;
}
s32 RotTransPers3(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, s32* sxy0, s32* sxy1, s32* sxy2, s32* p, s32* flag) {
    gte_load_sv(0, v0); gte_load_sv(1, v1); gte_load_sv(2, v2);
    gte_command(GTE_CMD_RTPT);
    *sxy0 = (s32)gte_mfc2(GTE_D_SXY0); *sxy1 = (s32)gte_mfc2(GTE_D_SXY1); *sxy2 = (s32)gte_mfc2(GTE_D_SXY2);
    *p = (s32)gte_mfc2(GTE_D_IR0); *flag = (s32)gte_cfc2(GTE_C_FLAG);
    return (s32)gte_mfc2(GTE_D_SZ3) >> 2;
}
s32 RotTransPers4(SVECTOR* v0, SVECTOR* v1, SVECTOR* v2, SVECTOR* v3, s32* sxy0, s32* sxy1, s32* sxy2, s32* sxy3, s32* p, s32* flag) {
    u32 first;
    gte_load_sv(0, v0); gte_load_sv(1, v1); gte_load_sv(2, v2);
    gte_command(GTE_CMD_RTPT);
    *sxy0 = (s32)gte_mfc2(GTE_D_SXY0); *sxy1 = (s32)gte_mfc2(GTE_D_SXY1); *sxy2 = (s32)gte_mfc2(GTE_D_SXY2);
    first = gte_cfc2(GTE_C_FLAG);
    gte_load_sv(0, v3);
    gte_command(GTE_CMD_RTPS);
    *sxy3 = (s32)gte_mfc2(GTE_D_SXY2); *p = (s32)gte_mfc2(GTE_D_IR0);
    *flag = (s32)(gte_cfc2(GTE_C_FLAG) | first);
    return (s32)gte_mfc2(GTE_D_SZ3) >> 2;
}
long NormalClip(long a, long b, long c) {
    gte_mtc2(GTE_D_SXY0, (u32)a); gte_mtc2(GTE_D_SXY2, (u32)c); gte_mtc2(GTE_D_SXY1, (u32)b);
    gte_command(GTE_CMD_NCLIP);
    return (long)gte_mfc2(GTE_D_MAC0);
}

/* ---------------------------------------------------------------------------------- matrix functions */
static s32 sat16(s32 v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
static s16 s16at(const void* m, int idx) { return ((const s16*)m)[idx]; }   /* element idx of the 3x3 (row-major, no padding) */

/* Column-wise product: out = left * right, computed as the hardware does (RT = left; each column of `right` through
 * MVMVA sf=1, IR saturated). `out` receives packed words like the retail store (m22 as a full sign-extended word). */
static void mul_columns(const MATRIX* left, const MATRIX* right, MATRIX* out) {
    s16 r[9];
    int i, j;
    u32* o = (u32*)out;
    load_rot(left);
    for (j = 0; j < 3; j++) {
        u32 xy = ((u32)(u16)s16at(right, 0 * 3 + j)) | ((u32)(u16)s16at(right, 1 * 3 + j) << 16);
        gte_mtc2(GTE_D_VXY0, xy); gte_mtc2(GTE_D_VZ0, (u32)(s32)s16at(right, 2 * 3 + j));
        gte_command(GTE_CMD_MUL_COL);
        for (i = 0; i < 3; i++) r[i * 3 + j] = (s16)gte_mfc2(GTE_D_IR1 + i);
    }
    o[0] = ((u32)(u16)r[0]) | ((u32)(u16)r[1] << 16);
    o[1] = ((u32)(u16)r[2]) | ((u32)(u16)r[3] << 16);
    o[2] = ((u32)(u16)r[4]) | ((u32)(u16)r[5] << 16);
    o[3] = ((u32)(u16)r[6]) | ((u32)(u16)r[7] << 16);
    o[4] = (u32)(s32)r[8];
}
void MulMatrix(MATRIX* left, MATRIX* right) { MATRIX tmp; mul_columns(left, right, &tmp); { int i; for (i = 0; i < 5; i++) ((u32*)left)[i] = ((u32*)&tmp)[i]; } }
void MulMatrix0(MATRIX* left, void* right, MATRIX* out) { MATRIX tmp; mul_columns(left, (const MATRIX*)right, &tmp); { int i; for (i = 0; i < 5; i++) ((u32*)out)[i] = ((u32*)&tmp)[i]; } }
void MulMatrix2(MATRIX* left, MATRIX* right) { MATRIX tmp; mul_columns(left, right, &tmp); { int i; for (i = 0; i < 5; i++) ((u32*)right)[i] = ((u32*)&tmp)[i]; } }

void TransMatrix(void* matrix, void* vector) {
    MATRIX* m = (MATRIX*)matrix; VECTOR* v = (VECTOR*)vector;
    m->t[0] = v->vx; m->t[1] = v->vy; m->t[2] = v->vz;
}

/* Scale: low-word products (as `multu` does) followed by an arithmetic >> 12; results are stored as packed halfwords. */
static s32 mulshift(s32 a, s32 b) { return (s32)((u32)a * (u32)b) >> 12; }
void ScaleMatrix(void* matrix, void* scale_ptr) {          /* columns: m[i][j] *= scale[j] */
    u32* w = (u32*)matrix; VECTOR* sc = (VECTOR*)scale_ptr;
    s32 sx = sc->vx, sy = sc->vy, sz = sc->vz;
    u32 p0 = w[0], p1 = w[1], p2 = w[2], p3 = w[3], p4 = w[4];
    w[0] = ((u32)mulshift((s16)p0, sx) & 0xffff) | ((u32)mulshift((s16)(p0 >> 16), sy) << 16);
    w[1] = ((u32)mulshift((s16)p1, sz) & 0xffff) | ((u32)mulshift((s16)(p1 >> 16), sx) << 16);
    w[2] = ((u32)mulshift((s16)p2, sy) & 0xffff) | ((u32)mulshift((s16)(p2 >> 16), sz) << 16);
    w[3] = ((u32)mulshift((s16)p3, sx) & 0xffff) | ((u32)mulshift((s16)(p3 >> 16), sy) << 16);
    w[4] = (u32)mulshift((s16)p4, sz);
}
void ScaleMatrixL(MATRIX* matrix, VECTOR* scale) {         /* rows: m[i][j] *= scale[i] */
    u32* w = (u32*)matrix;
    s32 sx = scale->vx, sy = scale->vy, sz = scale->vz;
    u32 p0 = w[0], p1 = w[1], p2 = w[2], p3 = w[3], p4 = w[4];
    w[0] = ((u32)mulshift((s16)p0, sx) & 0xffff) | ((u32)mulshift((s16)(p0 >> 16), sx) << 16);
    w[1] = ((u32)mulshift((s16)p1, sx) & 0xffff) | ((u32)mulshift((s16)(p1 >> 16), sy) << 16);
    w[2] = ((u32)mulshift((s16)p2, sy) & 0xffff) | ((u32)mulshift((s16)(p2 >> 16), sy) << 16);
    w[3] = ((u32)mulshift((s16)p3, sz) & 0xffff) | ((u32)mulshift((s16)(p3 >> 16), sz) << 16);
    w[4] = (u32)mulshift((s16)p4, sz);
}

/* Matrix stack in the game's own RAM (20 frames of a MATRIX); the retail code prints a message on overflow/underflow. */
void PushMatrix(void) {
    s32 off = g_psyq_gte_matrix_stack_offset;
    u32* f;
    int i;
    if (off >= 640) return;
    f = (u32*)((u8*)g_psyq_gte_matrix_stack + off);
    for (i = 0; i < 5; i++) f[i] = gte_cfc2(GTE_C_R11R12 + i);
    for (i = 0; i < 3; i++) f[5 + i] = gte_cfc2(GTE_C_TRX + i);
    g_psyq_gte_matrix_stack_offset = off + 32;
}
void PopMatrix(void) {
    s32 off = g_psyq_gte_matrix_stack_offset;
    u32* f;
    int i;
    if (off <= 0) return;
    off -= 32;
    g_psyq_gte_matrix_stack_offset = off;
    f = (u32*)((u8*)g_psyq_gte_matrix_stack + off);
    for (i = 0; i < 5; i++) gte_ctc2(GTE_C_R11R12 + i, f[i]);
    for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, f[5 + i]);
}

/* ApplyMatrixLV: rotate a 32-bit vector by splitting each component into sign-magnitude 15-bit halves, as the retail code does:
 * out = 8 * MVMVA_high(sf=0) + MVMVA_low(sf=1). */
VECTOR* ApplyMatrixLV(MATRIX* m, VECTOR* in, VECTOR* out) {
    s32 v[3], hi[3], lo[3], mh[3], ml[3];
    int k;
    load_rot(m);
    v[0] = in->vx; v[1] = in->vy; v[2] = in->vz;
    for (k = 0; k < 3; k++) {
        s32 a = v[k] < 0 ? -v[k] : v[k];
        hi[k] = a >> 15; lo[k] = a & 0x7fff;
        if (v[k] < 0) { hi[k] = -hi[k]; lo[k] = -lo[k]; }
    }
    gte_load_ir(0, 0, 0);
    gte_mtc2(GTE_D_IR1, (u32)hi[0]); gte_mtc2(GTE_D_IR2, (u32)hi[1]); gte_mtc2(GTE_D_IR3, (u32)hi[2]);
    gte_command(GTE_CMD_LV_HIGH);
    for (k = 0; k < 3; k++) mh[k] = (s32)gte_mfc2(GTE_D_MAC1 + k);
    gte_mtc2(GTE_D_IR1, (u32)lo[0]); gte_mtc2(GTE_D_IR2, (u32)lo[1]); gte_mtc2(GTE_D_IR3, (u32)lo[2]);
    gte_command(GTE_CMD_LV_LOW);
    for (k = 0; k < 3; k++) ml[k] = (s32)gte_mfc2(GTE_D_MAC1 + k);
    out->vx = ml[0] + mh[0] * 8; out->vy = ml[1] + mh[1] * 8; out->vz = ml[2] + mh[2] * 8;
    return out;
}

/* RotMatrix: build a rotation from three angles (12-bit turns) with the game's packed sin/cos table (cos in the high
 * halfword, sin in the low one). Products are low-word (`multu`) followed by an arithmetic >> 12, exactly as retail. */
static void sincos(s32 angle, s32* sn, s32* cs) {
    u32 e = g_psyq_gte_sin_cos_table[(angle < 0 ? -angle : angle) & 0xfff];
    s32 s = (s16)(e & 0xffff), c = (s16)(e >> 16);
    *sn = angle < 0 ? -s : s;
    *cs = c;
}
void RotMatrix(SVECTOR* r, MATRIX* m) {
    s32 sx, cx, sy, cy, sz, cz, negsy, t, a;
    sincos(r->vx, &sx, &cx); sincos(r->vy, &sy, &cy); sincos(r->vz, &sz, &cz);
    negsy = -sy;
    m->m[0][2] = (s16)sy;
    /* m[1][2] = (-(cy*sx)) >> 12 : negate the low product, then shift arithmetically */
    m->m[1][2] = (s16)(((s32)(0u - (u32)cy * (u32)sx)) >> 12);
    m->m[2][2] = (s16)mulshift(cy, cx);
    m->m[0][0] = (s16)mulshift(cz, cy);
    m->m[0][1] = (s16)(((s32)(0u - (u32)sz * (u32)cy)) >> 12);
    t = mulshift(cz, negsy);                                   /* t = (cz * -sy) >> 12 */
    m->m[1][0] = (s16)(mulshift(sz, cx) - mulshift(t, sx));
    m->m[2][0] = (s16)(mulshift(sz, sx) + mulshift(t, cx));
    a = mulshift(sz, negsy);                                   /* a = (sz * -sy) >> 12 */
    m->m[1][1] = (s16)(mulshift(cz, cx) + mulshift(a, sx));
    m->m[2][1] = (s16)(mulshift(cz, sx) - mulshift(a, cx));
}

/* VectorNormal: normalise a wide vector to length 4096 (retail worker: SQR on the low halves, LZCS-based exponent,
 * inverse-square-root table, GPF, arithmetic shift). */
void VectorNormal(VECTOR* in, VECTOR* out) {
    s32 x = in->vx, y = in->vy, z = in->vz, sum, leading, exponent, difference, normalized, factor;
    gte_mtc2(GTE_D_IR1, (u32)x); gte_mtc2(GTE_D_IR2, (u32)y); gte_mtc2(GTE_D_IR3, (u32)z);
    gte_command(GTE_CMD_SQR0);
    sum = (s32)((u32)gte_mfc2(GTE_D_MAC1) + (u32)gte_mfc2(GTE_D_MAC2) + (u32)gte_mfc2(GTE_D_MAC3));
    leading = Lzc(sum) & ~1;
    exponent = (31 - leading) >> 1;
    difference = leading - 24;
    if (difference >= 0) normalized = (s32)((u32)sum << difference);
    else normalized = sum >> (24 - leading);
    normalized -= 64;
    factor = g_psyq_gte_inv_sqrt_table[normalized];
    gte_mtc2(GTE_D_IR0, (u32)factor);
    gte_mtc2(GTE_D_IR1, (u32)x); gte_mtc2(GTE_D_IR2, (u32)y); gte_mtc2(GTE_D_IR3, (u32)z);
    gte_command(GTE_CMD_GPF12);
    out->vx = (s32)gte_mfc2(GTE_D_MAC1) >> exponent;
    out->vy = (s32)gte_mfc2(GTE_D_MAC2) >> exponent;
    out->vz = (s32)gte_mfc2(GTE_D_MAC3) >> exponent;
}

/* SquareRoot0 / SquareRoot12: table approximation with signed normalisation (LZCS parity trick). */
static long sqrt_common(long value, int base, int shift12) {
    s32 lz = Lzc((s32)value), even, exponent, difference, normalized, factor;
    if (lz == 32) return 0;
    even = lz & ~1;
    exponent = (base - even) >> 1;
    difference = even - 24;
    if (difference >= 0) normalized = (s32)((u32)value << difference);
    else normalized = (s32)value >> (24 - even);
    normalized -= 64;
    factor = g_psyq_gte_sqrt_table[normalized];
    if (shift12) return (long)(((u32)factor << exponent) >> 12);
    return exponent < 0 ? (long)((u32)factor >> -exponent) : (long)((u32)factor << exponent);
}
long SquareRoot0(long value) { return sqrt_common(value, 31, 1); }
s32 SquareRoot12(s32 value) { return (s32)sqrt_common(value, 19, 0); }
