/* Software GTE core -- see gte.h. Freestanding, deterministic, integer only. */
#include "gte.h"

gte_state_t g_gte;

/* ----------------------------------------------------------------------------------------------- helpers */
static gte_s16 lo16(gte_u32 v) { return (gte_s16)(v & 0xffff); }
static gte_s16 hi16(gte_u32 v) { return (gte_s16)(v >> 16); }
static gte_u32 pack16(gte_s32 lo, gte_s32 hi) { return ((gte_u32)hi << 16) | ((gte_u32)lo & 0xffff); }

static gte_s32 clz32(gte_u32 x) { return x ? __builtin_clz(x) : 32; }

static void set_flag(gte_u32 f) { g_gte.flag |= f; }

void gte_reset(void) {
    int i;
    for (i = 0; i < 32; i++) { g_gte.ctrl[i] = 0; g_gte.data[i] = 0; }
    g_gte.flag = 0;
    g_gte.data[GTE_D_LZCR] = 32;
}

/* ------------------------------------------------------------------------------------ register access */
void gte_ctc2(int reg, gte_u32 v) {
    reg &= 31;
    switch (reg) {
    case GTE_C_R33: case GTE_C_L33: case GTE_C_LB3:
    case GTE_C_H: case GTE_C_DQA: case GTE_C_ZSF3: case GTE_C_ZSF4:
        g_gte.ctrl[reg] = (gte_s32)lo16(v);            /* 16-bit registers keep their sign-extended value */
        break;
    case GTE_C_FLAG:
        g_gte.flag = v & 0x7ffff000u;                    /* bits 30..12 are writable */
        break;
    default:
        g_gte.ctrl[reg] = (gte_s32)v;
        break;
    }
}

gte_u32 gte_cfc2(int reg) {
    reg &= 31;
    if (reg == GTE_C_FLAG) {
        gte_u32 f = g_gte.flag & 0x7ffff000u;
        if (f & 0x7f87e000u) f |= GTE_F_ERROR;           /* error = any of bits 30..23 and 18..13 */
        return f;
    }
    return (gte_u32)g_gte.ctrl[reg];                     /* H is returned sign-extended, as the hardware does */
}

static gte_s32 sat5(gte_s32 v) { v >>= 7; return v < 0 ? 0 : (v > 0x1f ? 0x1f : v); }

void gte_mtc2(int reg, gte_u32 v) {
    reg &= 31;
    switch (reg) {
    case GTE_D_SXYP:                                    /* writing SXYP pushes the screen-XY FIFO */
        g_gte.data[GTE_D_SXY0] = g_gte.data[GTE_D_SXY1];
        g_gte.data[GTE_D_SXY1] = g_gte.data[GTE_D_SXY2];
        g_gte.data[GTE_D_SXY2] = (gte_s32)v;
        break;
    case GTE_D_IRGB:
        g_gte.data[GTE_D_IR1] = (gte_s32)((v & 0x1f) * 0x80);
        g_gte.data[GTE_D_IR2] = (gte_s32)(((v >> 5) & 0x1f) * 0x80);
        g_gte.data[GTE_D_IR3] = (gte_s32)(((v >> 10) & 0x1f) * 0x80);
        break;
    case GTE_D_ORGB: case GTE_D_LZCR: break;               /* read-only */
    case GTE_D_LZCS:
        g_gte.data[GTE_D_LZCS] = (gte_s32)v;
        g_gte.data[GTE_D_LZCR] = (gte_s32)((gte_s32)v < 0 ? clz32(~v) : clz32(v));
        break;
    case GTE_D_OTZ: case GTE_D_SZ0: case GTE_D_SZ1: case GTE_D_SZ2: case GTE_D_SZ3:
        g_gte.data[reg] = (gte_s32)(v & 0xffff);
        break;
    case GTE_D_IR0: case GTE_D_IR1: case GTE_D_IR2: case GTE_D_IR3:
        g_gte.data[reg] = (gte_s32)lo16(v);
        break;
    default:
        g_gte.data[reg] = (gte_s32)v;
        break;
    }
}

gte_u32 gte_mfc2(int reg) {
    reg &= 31;
    switch (reg) {
    case GTE_D_VZ0: case GTE_D_VZ1: case GTE_D_VZ2: return (gte_u32)(gte_s32)lo16((gte_u32)g_gte.data[reg]);
    case GTE_D_OTZ: case GTE_D_SZ0: case GTE_D_SZ1: case GTE_D_SZ2: case GTE_D_SZ3: return (gte_u32)g_gte.data[reg] & 0xffff;
    case GTE_D_IR0: case GTE_D_IR1: case GTE_D_IR2: case GTE_D_IR3: return (gte_u32)(gte_s32)lo16((gte_u32)g_gte.data[reg]);
    case GTE_D_SXYP: return (gte_u32)g_gte.data[GTE_D_SXY2];
    case GTE_D_IRGB: case GTE_D_ORGB:
        return (gte_u32)(sat5(g_gte.data[GTE_D_IR1]) | (sat5(g_gte.data[GTE_D_IR2]) << 5) | (sat5(g_gte.data[GTE_D_IR3]) << 10));
    default: return (gte_u32)g_gte.data[reg];
    }
}

void gte_load_sv(int v, const void* p) {
    const gte_u32* w = (const gte_u32*)p;
    gte_mtc2(2 * v, w[0]);
    gte_mtc2(2 * v + 1, w[1]);
}
void gte_load_ir(gte_s32 x, gte_s32 y, gte_s32 z) {
    g_gte.data[GTE_D_IR1] = x; g_gte.data[GTE_D_IR2] = y; g_gte.data[GTE_D_IR3] = z;
}

/* ------------------------------------------------------------------------------------------ commands */
#define MAC(n) g_gte.data[GTE_D_MAC0 + (n)]
#define IRN(n) g_gte.data[GTE_D_IR0 + (n)]

/* 44-bit overflow check on a multiply-accumulate; the value itself is kept (we hold it in 64 bits) */
static gte_s64 mac_check(int n, gte_s64 v) {
    static const gte_u32 of[4] = { 0, GTE_F_MAC1_OF, GTE_F_MAC2_OF, GTE_F_MAC3_OF };
    static const gte_u32 uf[4] = { 0, GTE_F_MAC1_UF, GTE_F_MAC2_UF, GTE_F_MAC3_UF };
    if (v > 0x7ffffffffffLL) set_flag(of[n]);
    else if (v < -0x80000000000LL) set_flag(uf[n]);
    return v;
}

static gte_s32 sat_ir(int n, gte_s32 v, int lm) {
    static const gte_u32 sat[4] = { GTE_F_IR0_SAT, GTE_F_IR1_SAT, GTE_F_IR2_SAT, GTE_F_IR3_SAT };
    gte_s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { set_flag(sat[n]); return lo; }
    if (v > 0x7fff) { set_flag(sat[n]); return 0x7fff; }
    return v;
}

static gte_s16 rtm(int reg, int idx) {  /* element idx (0..8, row-major) of the 3x3 matrix packed in ctrl[reg..reg+4] */
    gte_u32 w = (gte_u32)g_gte.ctrl[reg + idx / 2];      /* word k holds elements 2k (low half) and 2k+1 (high half) */
    return (idx & 1) ? hi16(w) : lo16(w);
}

static void mvmva(gte_u32 cmd) {
    int sf = GTE_CMD_SF(cmd) * 12, lm = GTE_CMD_LM(cmd), mx = GTE_CMD_MX(cmd), vx = GTE_CMD_VX(cmd), tx = GTE_CMD_TX(cmd);
    gte_s32 m[3][3], v[3], t[3];
    int i, j;
    if (mx == 3) {  /* reserved "garbage" matrix (hardware quirk): (-R<<4, R<<4, IR0, RT13, RT13, RT13, RT22, RT22, RT22) */
        gte_s32 r = (gte_s32)((gte_u32)g_gte.data[GTE_D_RGBC] & 0xff);
        m[0][0] = -(r << 4); m[0][1] = r << 4; m[0][2] = IRN(0);
        for (j = 0; j < 3; j++) { m[1][j] = rtm(GTE_C_R11R12, 2); m[2][j] = rtm(GTE_C_R11R12, 4); }
    } else {
        int base = mx == 0 ? GTE_C_R11R12 : (mx == 1 ? GTE_C_L11L12 : GTE_C_LR1LR2);
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) m[i][j] = rtm(base, i * 3 + j);
    }
    if (vx == 3) { v[0] = IRN(1); v[1] = IRN(2); v[2] = IRN(3); }
    else {
        gte_u32 xy = (gte_u32)g_gte.data[2 * vx], z = (gte_u32)g_gte.data[2 * vx + 1];
        v[0] = lo16(xy); v[1] = hi16(xy); v[2] = lo16(z);
    }
    if (tx == 3) { t[0] = t[1] = t[2] = 0; }
    else {
        int base = tx == 0 ? GTE_C_TRX : (tx == 1 ? GTE_C_RBK : GTE_C_RFC);
        for (i = 0; i < 3; i++) t[i] = g_gte.ctrl[base + i];
    }
    for (i = 0; i < 3; i++) {
        gte_s64 sum = (gte_s64)t[i] * 0x1000 + (gte_s64)m[i][0] * v[0] + (gte_s64)m[i][1] * v[1] + (gte_s64)m[i][2] * v[2];
        MAC(i + 1) = (gte_s32)(mac_check(i + 1, sum) >> sf);
    }
    for (i = 1; i <= 3; i++) IRN(i) = sat_ir(i, MAC(i), lm);
}

/* Perspective divide n = ((H * 0x20000 / SZ3) + 1) / 2, capped at 0x1ffff. Two implementations of the quotient (both assume
 * SZ3 != 0 and H < 2 * SZ3, which divide() has already checked):
 *   gte_divide_unr   the hardware's algorithm as documented in psx-spx: a 257-entry reciprocal table (generated below), two
 *                    Newton steps and a final multiply (default; not yet compared against a console/emulator trace);
 *   gte_divide_exact true integer division, in 32-bit steps (no libgcc). Differs from the hardware by at most 1 in rare cases.
 * The two are exported so the unit tests can cross-check one against the other. */
static gte_u32 unr_table[0x101];
static int unr_ready;
static void unr_init(void) {
    int i;
    for (i = 0; i <= 0x100; i++) {
        gte_s32 v = ((0x40000 / (i + 0x100) + 1) / 2) - 0x101;
        unr_table[i] = v < 0 ? 0 : (gte_u32)v;
    }
    unr_ready = 1;
}
gte_u32 gte_divide_unr(gte_u32 h, gte_u32 sz3) {
    gte_u32 z = 0, n, d, u;
    gte_u64 q;
    if (!unr_ready) unr_init();
    while (z < 16 && !(sz3 & (0x8000u >> z))) z++;          /* leading zeros of the 16-bit SZ3 */
    n = h << z;
    d = sz3 << z;
    u = unr_table[(d - 0x7fc0u) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    q = ((gte_u64)n * d + 0x8000u) >> 16;
    return q > 0x1ffffu ? 0x1ffffu : (gte_u32)q;
}
gte_u32 gte_divide_exact(gte_u32 h, gte_u32 sz3) {          /* floor(h * 2^17 / sz3) in two 32-bit long-division steps */
    gte_u32 q1 = (h << 8) / sz3, r1 = (h << 8) % sz3;         /* h <= 0xffff, so h << 8 fits */
    gte_u32 q = (q1 << 9) + ((r1 << 9) / sz3);                /* r1 < sz3 <= 0xffff */
    q = (q + 1) / 2;
    return q > 0x1ffffu ? 0x1ffffu : q;
}
static gte_s32 divide(gte_u32 h, gte_u32 sz3) {
    if (sz3 == 0 || h >= sz3 * 2) { set_flag(GTE_F_DIV_OF); return 0x1ffff; }
#ifdef GTE_EXACT_DIVIDE
    return (gte_s32)gte_divide_exact(h, sz3);
#else
    return (gte_s32)gte_divide_unr(h, sz3);
#endif
}

static void rtp(int vertex, int sf12, int lm) {   /* sf12 = 0 or 12: the MAC shift amount */
    gte_u32 xy = (gte_u32)g_gte.data[2 * vertex], z = (gte_u32)g_gte.data[2 * vertex + 1];
    gte_s32 v[3], i;
    gte_s64 s;
    gte_s32 sz, n;
    v[0] = lo16(xy); v[1] = hi16(xy); v[2] = lo16(z);
    for (i = 0; i < 3; i++) {
        gte_s64 sum = (gte_s64)g_gte.ctrl[GTE_C_TRX + i] * 0x1000
                    + (gte_s64)rtm(GTE_C_R11R12, i * 3 + 0) * v[0] + (gte_s64)rtm(GTE_C_R11R12, i * 3 + 1) * v[1]
                    + (gte_s64)rtm(GTE_C_R11R12, i * 3 + 2) * v[2];
        MAC(i + 1) = (gte_s32)(mac_check(i + 1, sum) >> sf12);
    }
    IRN(1) = sat_ir(1, MAC(1), lm);
    IRN(2) = sat_ir(2, MAC(2), lm);
    IRN(3) = sat_ir(3, MAC(3), lm);
    /* SZ FIFO push; SZ3 = MAC3 >> ((1 - sf) * 12) i.e. 12 - sf12, clamped to 0..0xffff */
    sz = (gte_s32)((gte_s64)MAC(3) >> (12 - sf12));
    if (sz < 0) { set_flag(GTE_F_SZ3_OTZ_SAT); sz = 0; } else if (sz > 0xffff) { set_flag(GTE_F_SZ3_OTZ_SAT); sz = 0xffff; }
    g_gte.data[GTE_D_SZ0] = g_gte.data[GTE_D_SZ1];
    g_gte.data[GTE_D_SZ1] = g_gte.data[GTE_D_SZ2];
    g_gte.data[GTE_D_SZ2] = g_gte.data[GTE_D_SZ3];
    g_gte.data[GTE_D_SZ3] = sz;
    n = divide((gte_u32)g_gte.ctrl[GTE_C_H] & 0xffff, (gte_u32)sz);
    {
        gte_s32 sx, sy;
        s = (gte_s64)n * IRN(1) + g_gte.ctrl[GTE_C_OFX];
        if (s > 0x7fffffffLL) set_flag(GTE_F_MAC0_OF); else if (s < -0x80000000LL) set_flag(GTE_F_MAC0_UF);
        sx = (gte_s32)(s >> 16);
        if (sx < -0x400) { set_flag(GTE_F_SX2_SAT); sx = -0x400; } else if (sx > 0x3ff) { set_flag(GTE_F_SX2_SAT); sx = 0x3ff; }
        s = (gte_s64)n * IRN(2) + g_gte.ctrl[GTE_C_OFY];
        if (s > 0x7fffffffLL) set_flag(GTE_F_MAC0_OF); else if (s < -0x80000000LL) set_flag(GTE_F_MAC0_UF);
        sy = (gte_s32)(s >> 16);
        if (sy < -0x400) { set_flag(GTE_F_SY2_SAT); sy = -0x400; } else if (sy > 0x3ff) { set_flag(GTE_F_SY2_SAT); sy = 0x3ff; }
        g_gte.data[GTE_D_SXY0] = g_gte.data[GTE_D_SXY1];
        g_gte.data[GTE_D_SXY1] = g_gte.data[GTE_D_SXY2];
        g_gte.data[GTE_D_SXY2] = (gte_s32)pack16(sx, sy);
    }
    s = (gte_s64)n * g_gte.ctrl[GTE_C_DQA] + g_gte.ctrl[GTE_C_DQB];
    if (s > 0x7fffffffLL) set_flag(GTE_F_MAC0_OF); else if (s < -0x80000000LL) set_flag(GTE_F_MAC0_UF);
    MAC(0) = (gte_s32)s;
    {
        gte_s32 ir0 = (gte_s32)(s >> 12);
        if (ir0 < 0) { set_flag(GTE_F_IR0_SAT); ir0 = 0; } else if (ir0 > 0x1000) { set_flag(GTE_F_IR0_SAT); ir0 = 0x1000; }
        IRN(0) = ir0;
    }
}

static void set_otz(gte_s64 mac0) {
    gte_s32 v;
    if (mac0 > 0x7fffffffLL) set_flag(GTE_F_MAC0_OF); else if (mac0 < -0x80000000LL) set_flag(GTE_F_MAC0_UF);
    MAC(0) = (gte_s32)mac0;
    v = (gte_s32)(mac0 >> 12);
    if (v < 0) { set_flag(GTE_F_SZ3_OTZ_SAT); v = 0; } else if (v > 0xffff) { set_flag(GTE_F_SZ3_OTZ_SAT); v = 0xffff; }
    g_gte.data[GTE_D_OTZ] = v;
}

void gte_command(gte_u32 cmd) {
    int sf = GTE_CMD_SF(cmd) * 12, lm = GTE_CMD_LM(cmd), i;
    cmd &= 0x1ffffff;
    g_gte.flag = 0;
    switch (GTE_CMD_OP(cmd)) {
    case GTE_OP_MVMVA: mvmva(cmd); break;
    case GTE_OP_RTPS:  rtp(0, sf, lm); break;
    case GTE_OP_RTPT:  rtp(0, sf, lm); rtp(1, sf, lm); rtp(2, sf, lm); break;
    case GTE_OP_NCLIP: {
        gte_s32 x0 = lo16((gte_u32)g_gte.data[GTE_D_SXY0]), y0 = hi16((gte_u32)g_gte.data[GTE_D_SXY0]);
        gte_s32 x1 = lo16((gte_u32)g_gte.data[GTE_D_SXY1]), y1 = hi16((gte_u32)g_gte.data[GTE_D_SXY1]);
        gte_s32 x2 = lo16((gte_u32)g_gte.data[GTE_D_SXY2]), y2 = hi16((gte_u32)g_gte.data[GTE_D_SXY2]);
        gte_s64 s = (gte_s64)x0 * y1 + (gte_s64)x1 * y2 + (gte_s64)x2 * y0 - (gte_s64)x0 * y2 - (gte_s64)x1 * y0 - (gte_s64)x2 * y1;
        if (s > 0x7fffffffLL) set_flag(GTE_F_MAC0_OF); else if (s < -0x80000000LL) set_flag(GTE_F_MAC0_UF);
        MAC(0) = (gte_s32)s;
        break;
    }
    case GTE_OP_SQR:
        for (i = 1; i <= 3; i++) {
            gte_s64 s = (gte_s64)IRN(i) * IRN(i);
            MAC(i) = (gte_s32)(mac_check(i, s) >> sf);
        }
        for (i = 1; i <= 3; i++) IRN(i) = sat_ir(i, MAC(i), lm);
        break;
    case GTE_OP_OP: {
        gte_s32 d1 = rtm(GTE_C_R11R12, 0), d2 = rtm(GTE_C_R11R12, 4), d3 = rtm(GTE_C_R11R12, 8);
        gte_s32 i1 = IRN(1), i2 = IRN(2), i3 = IRN(3);
        MAC(1) = (gte_s32)(mac_check(1, (gte_s64)d2 * i3 - (gte_s64)d3 * i2) >> sf);
        MAC(2) = (gte_s32)(mac_check(2, (gte_s64)d3 * i1 - (gte_s64)d1 * i3) >> sf);
        MAC(3) = (gte_s32)(mac_check(3, (gte_s64)d1 * i2 - (gte_s64)d2 * i1) >> sf);
        for (i = 1; i <= 3; i++) IRN(i) = sat_ir(i, MAC(i), lm);
        break;
    }
    case GTE_OP_AVSZ3:
        set_otz((gte_s64)(gte_s16)g_gte.ctrl[GTE_C_ZSF3] * ((gte_s64)(gte_u32)(g_gte.data[GTE_D_SZ1] & 0xffff)
                + (gte_u32)(g_gte.data[GTE_D_SZ2] & 0xffff) + (gte_u32)(g_gte.data[GTE_D_SZ3] & 0xffff)));
        break;
    case GTE_OP_AVSZ4:
        set_otz((gte_s64)(gte_s16)g_gte.ctrl[GTE_C_ZSF4] * ((gte_s64)(gte_u32)(g_gte.data[GTE_D_SZ0] & 0xffff)
                + (gte_u32)(g_gte.data[GTE_D_SZ1] & 0xffff) + (gte_u32)(g_gte.data[GTE_D_SZ2] & 0xffff)
                + (gte_u32)(g_gte.data[GTE_D_SZ3] & 0xffff)));
        break;
    case GTE_OP_GPF: {                                      /* general purpose interpolation: MACn = IR0 * IRn >> sf, pushed to the colour FIFO */
        static const gte_u32 rgb_sat[3] = { GTE_F_RGB_R_SAT, GTE_F_RGB_G_SAT, GTE_F_RGB_B_SAT };
        gte_u32 rgb = (gte_u32)g_gte.data[GTE_D_RGBC] & 0xff000000u;
        for (i = 1; i <= 3; i++) {
            gte_s64 s = (gte_s64)IRN(0) * IRN(i);
            MAC(i) = (gte_s32)(mac_check(i, s) >> sf);
        }
        for (i = 1; i <= 3; i++) {
            gte_s32 c = MAC(i) >> 4;
            if (c < 0) { set_flag(rgb_sat[i - 1]); c = 0; } else if (c > 0xff) { set_flag(rgb_sat[i - 1]); c = 0xff; }
            rgb |= (gte_u32)c << (8 * (i - 1));
        }
        g_gte.data[GTE_D_RGB0] = g_gte.data[GTE_D_RGB1];
        g_gte.data[GTE_D_RGB1] = g_gte.data[GTE_D_RGB2];
        g_gte.data[GTE_D_RGB2] = (gte_s32)rgb;
        for (i = 1; i <= 3; i++) IRN(i) = sat_ir(i, MAC(i), lm);
        break;
    }
    default:
        break;   /* colour/lighting commands are not used by the game's libgte API; unimplemented on purpose */
    }
}
