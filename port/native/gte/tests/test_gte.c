/* Known-answer and randomized tests for the software GTE core. Hosted 64-bit build: gcc -O1 test_gte.c ../gte.c */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../gte.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static gte_u32 pk(int lo, int hi) { return ((gte_u32)hi << 16) | ((gte_u32)lo & 0xffff); }

static void set_rt(const int m[3][3]) {
    gte_ctc2(GTE_C_R11R12, pk(m[0][0], m[0][1])); gte_ctc2(GTE_C_R13R21, pk(m[0][2], m[1][0]));
    gte_ctc2(GTE_C_R22R23, pk(m[1][1], m[1][2])); gte_ctc2(GTE_C_R31R32, pk(m[2][0], m[2][1]));
    gte_ctc2(GTE_C_R33, (gte_u32)m[2][2]);
}
static void set_v(int v, int x, int y, int z) { gte_mtc2(2 * v, pk(x, y)); gte_mtc2(2 * v + 1, (gte_u32)z); }
static long long floordiv(long long a, long long b) { long long q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static int sat16(long long v, int lm) { long long lo = lm ? 0 : -32768; return v < lo ? lo : (v > 32767 ? 32767 : (int)v); }

static void test_identity_and_rotation(void) {
    const int I[3][3] = { {4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096} };
    const int Rz90[3][3] = { {0, -4096, 0}, {4096, 0, 0}, {0, 0, 4096} };
    gte_reset(); set_rt(I);
    gte_ctc2(GTE_C_TRX, 10); gte_ctc2(GTE_C_TRY, 20); gte_ctc2(GTE_C_TRZ, 30);
    set_v(0, 1, 2, 3);
    gte_command(0x4a480012);                                   /* RTV0TR = MVMVA sf=1 mx=RT vx=V0 tx=TR */
    CHECK(gte_mfc2(GTE_D_MAC1) == 11 && gte_mfc2(GTE_D_MAC2) == 22 && gte_mfc2(GTE_D_MAC3) == 33, "identity+TR: %d %d %d",
          (int)gte_mfc2(GTE_D_MAC1), (int)gte_mfc2(GTE_D_MAC2), (int)gte_mfc2(GTE_D_MAC3));
    gte_reset(); set_rt(Rz90); set_v(0, 100, 0, 0);
    gte_command(0x4a480012);
    CHECK((int)gte_mfc2(GTE_D_MAC1) == 0 && (int)gte_mfc2(GTE_D_MAC2) == 100 && (int)gte_mfc2(GTE_D_MAC3) == 0, "Rz90: %d %d %d",
          (int)gte_mfc2(GTE_D_MAC1), (int)gte_mfc2(GTE_D_MAC2), (int)gte_mfc2(GTE_D_MAC3));
}

static void test_mvmva_random(void) {
    int trial, i, j;
    srand(1234);
    for (trial = 0; trial < 20000; trial++) {
        int m[3][3], v[3], tr[3], sf = rand() & 1, lm = rand() & 1;
        long long ref[3];
        gte_u32 cmd = 0x4a400012u | ((gte_u32)sf << 19) | ((gte_u32)lm << 10);   /* mx=RT vx=V0 tx=TR */
        gte_reset();
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) m[i][j] = (rand() % 16384) - 8192;
        for (i = 0; i < 3; i++) { v[i] = (rand() % 65536) - 32768; tr[i] = (rand() % 200001) - 100000; }
        set_rt((const int(*)[3])m); set_v(0, v[0], v[1], v[2]);
        gte_ctc2(GTE_C_TRX, (gte_u32)tr[0]); gte_ctc2(GTE_C_TRY, (gte_u32)tr[1]); gte_ctc2(GTE_C_TRZ, (gte_u32)tr[2]);
        gte_command(cmd);
        for (i = 0; i < 3; i++) {
            long long s = (long long)tr[i] * 4096 + (long long)m[i][0] * v[0] + (long long)m[i][1] * v[1] + (long long)m[i][2] * v[2];
            ref[i] = floordiv(s, sf ? 4096 : 1);
            CHECK((int)gte_mfc2(GTE_D_MAC1 + i) == (int)ref[i], "MAC%d trial %d: %d vs %lld", i + 1, trial, (int)gte_mfc2(GTE_D_MAC1 + i), ref[i]);
            CHECK((int)gte_mfc2(GTE_D_IR1 + i) == sat16(ref[i], lm), "IR%d trial %d: %d vs %d", i + 1, trial, (int)gte_mfc2(GTE_D_IR1 + i), sat16(ref[i], lm));
        }
        if (fails > 20) return;
    }
}

static void test_rtps(void) {
    int trial;
    srand(99);
    for (trial = 0; trial < 5000; trial++) {
        int H = 200 + rand() % 800, x = (rand() % 800) - 400, y = (rand() % 800) - 400, z = 200 + rand() % 3000;
        int ofx = 160, ofy = 120;
        const int I[3][3] = { {4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096} };
        double fx, fy;
        int sx, sy;
        gte_reset(); set_rt(I);
        gte_ctc2(GTE_C_OFX, (gte_u32)(ofx << 16)); gte_ctc2(GTE_C_OFY, (gte_u32)(ofy << 16)); gte_ctc2(GTE_C_H, (gte_u32)H);
        gte_ctc2(GTE_C_DQA, 0); gte_ctc2(GTE_C_DQB, 0);
        set_v(0, x, y, z);
        gte_command(0x4a180001);                                /* RTPS sf=1 */
        sx = (short)(gte_mfc2(GTE_D_SXY2) & 0xffff); sy = (short)(gte_mfc2(GTE_D_SXY2) >> 16);
        fx = ofx + (double)H * x / z; fy = ofy + (double)H * y / z;
        if (fx > -1024 && fx < 1023 && fy > -1024 && fy < 1023 && z >= H / 2 + 1) {
            CHECK(abs(sx - (int)floor(fx)) <= 1 && abs(sy - (int)floor(fy)) <= 1, "RTPS trial %d: got (%d,%d) want ~(%.2f,%.2f) H=%d v=(%d,%d,%d)", trial, sx, sy, fx, fy, H, x, y, z);
            CHECK((int)gte_mfc2(GTE_D_SZ3) == z, "SZ3 %d vs %d", (int)gte_mfc2(GTE_D_SZ3), z);
        }
        if (fails > 20) return;
    }
}

static void test_rtpt_fifo_nclip_avsz(void) {
    const int I[3][3] = { {4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096} };
    int sxy[3][2], k;
    gte_reset(); set_rt(I);
    gte_ctc2(GTE_C_H, 500); gte_ctc2(GTE_C_OFX, 0); gte_ctc2(GTE_C_OFY, 0);
    gte_ctc2(GTE_C_ZSF3, 0x155); gte_ctc2(GTE_C_ZSF4, 0x100);   /* ZSF3 ~ 4096/12? (just non-trivial) */
    set_v(0, 0, 0, 500); set_v(1, 100, 0, 500); set_v(2, 0, 100, 500);   /* CCW-ish in screen space: (0,0) (100,0) (0,100) */
    gte_command(0x4a280030);                                    /* RTPT */
    for (k = 0; k < 3; k++) {
        gte_u32 w = gte_mfc2(GTE_D_SXY0 + k);
        sxy[k][0] = (short)(w & 0xffff); sxy[k][1] = (short)(w >> 16);
    }
    CHECK(sxy[0][0] == 0 && sxy[0][1] == 0 && sxy[1][0] == 100 && sxy[1][1] == 0 && sxy[2][0] == 0 && sxy[2][1] == 100,
          "RTPT FIFO: (%d,%d) (%d,%d) (%d,%d)", sxy[0][0], sxy[0][1], sxy[1][0], sxy[1][1], sxy[2][0], sxy[2][1]);
    gte_command(0x4b400006);                                    /* NCLIP */
    CHECK((int)gte_mfc2(GTE_D_MAC0) == 100 * 100, "NCLIP area*2 = %d (expect 10000)", (int)gte_mfc2(GTE_D_MAC0));
    /* swap two vertices: winding flips sign */
    set_v(0, 0, 0, 500); set_v(1, 0, 100, 500); set_v(2, 100, 0, 500);
    gte_command(0x4a280030); gte_command(0x4b400006);
    CHECK((int)gte_mfc2(GTE_D_MAC0) == -10000, "NCLIP flipped = %d", (int)gte_mfc2(GTE_D_MAC0));
    /* AVSZ3: OTZ = ZSF3 * (SZ1+SZ2+SZ3) >> 12 = 0x155 * 1500 >> 12 */
    gte_command(0x4b58002d);
    CHECK((int)gte_mfc2(GTE_D_OTZ) == (0x155 * 1500) >> 12, "AVSZ3 %d vs %d", (int)gte_mfc2(GTE_D_OTZ), (0x155 * 1500) >> 12);
    gte_command(0x4b68002e);                                    /* AVSZ4 uses SZ0..SZ3 */
    CHECK((int)gte_mfc2(GTE_D_OTZ) == (0x100 * 2000) >> 12, "AVSZ4 %d vs %d", (int)gte_mfc2(GTE_D_OTZ), (0x100 * 2000) >> 12);
}

static void test_registers_and_flags(void) {
    gte_reset();
    gte_mtc2(GTE_D_LZCS, 0);           CHECK((int)gte_mfc2(GTE_D_LZCR) == 32, "lzcr(0)=%d", (int)gte_mfc2(GTE_D_LZCR));
    gte_mtc2(GTE_D_LZCS, 1);           CHECK((int)gte_mfc2(GTE_D_LZCR) == 31, "lzcr(1)=%d", (int)gte_mfc2(GTE_D_LZCR));
    gte_mtc2(GTE_D_LZCS, 0x80000000u); CHECK((int)gte_mfc2(GTE_D_LZCR) == 1, "lzcr(min)=%d", (int)gte_mfc2(GTE_D_LZCR));
    gte_mtc2(GTE_D_LZCS, (gte_u32)-1); CHECK((int)gte_mfc2(GTE_D_LZCR) == 32, "lzcr(-1)=%d", (int)gte_mfc2(GTE_D_LZCR));
    gte_mtc2(GTE_D_SXYP, pk(1, 2)); gte_mtc2(GTE_D_SXYP, pk(3, 4)); gte_mtc2(GTE_D_SXYP, pk(5, 6));
    CHECK(gte_mfc2(GTE_D_SXY0) == pk(1, 2) && gte_mfc2(GTE_D_SXY1) == pk(3, 4) && gte_mfc2(GTE_D_SXY2) == pk(5, 6), "SXYP FIFO");
    gte_mtc2(GTE_D_IRGB, 0x7fff);
    CHECK((int)gte_mfc2(GTE_D_IR1) == 31 * 128 && (int)gte_mfc2(GTE_D_IR3) == 31 * 128 && gte_mfc2(GTE_D_ORGB) == 0x7fff, "IRGB/ORGB");
    /* overflow flag: huge translation with sf=0 saturates IR and sets flags + the error bit */
    {
        const int I[3][3] = { {4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096} };
        gte_reset(); set_rt(I); gte_ctc2(GTE_C_TRX, 0x7fffffff); set_v(0, 30000, 0, 0);
        gte_command(0x4a400012);                                 /* sf=0: MAC1 = TRX*4096 + ... -> beyond 44 bits? */
        CHECK((gte_cfc2(GTE_C_FLAG) & GTE_F_ERROR) != 0, "error bit set on overflow, flag=%08x", gte_cfc2(GTE_C_FLAG));
        CHECK((gte_cfc2(GTE_C_FLAG) & GTE_F_IR1_SAT) != 0, "IR1 saturated flag");
        CHECK((int)gte_mfc2(GTE_D_IR1) == 32767, "IR1 = %d", (int)gte_mfc2(GTE_D_IR1));
    }
    /* H reads back sign-extended (hardware behaviour) */
    gte_ctc2(GTE_C_H, 0xffff); CHECK((int)gte_cfc2(GTE_C_H) == -1, "H sign-extended read");
}

/* The two perspective-divide implementations: `exact` must equal the 64-bit reference bit for bit; the hardware-style
 * reciprocal-table version (`unr`) is an approximation (relative error below 3e-5: 1 count off for small quotients, 3-4 near
 * the 0x1ffff cap) and must stay inside that bound (a wrong table entry or shift would blow well past it). */
static void test_divide(void) {
    gte_u32 sz3, k;
    long long total = 0, differ = 0;
    int maxerr = 0;
    double maxrel = 0;
    srand(7);
    for (sz3 = 1; sz3 <= 0xffff; sz3++) {
        for (k = 0; k < 40; k++) {
            gte_u32 h = k == 0 ? 0 : (k == 1 ? 1 : (k == 2 ? sz3 : (k == 3 ? 2 * sz3 - 1 : (gte_u32)rand() % (2 * sz3))));
            unsigned long long ref;
            gte_u32 e, u;
            if (h >= 2 * sz3 || h > 0xffff) continue;
            ref = (((unsigned long long)h * 0x20000u) / sz3 + 1) / 2;
            if (ref > 0x1ffff) ref = 0x1ffff;
            e = gte_divide_exact(h, sz3);
            u = gte_divide_unr(h, sz3);
            CHECK(e == ref, "divide exact h=%u sz3=%u: %u vs %llu", h, sz3, e, ref);
            CHECK(abs((int)u - (int)e) <= 1 + (int)(e / 32768), "divide unr h=%u sz3=%u: %u vs exact %u", h, sz3, u, e);
            total++; differ += (u != e);
            if (abs((int)u - (int)e) > maxerr) maxerr = abs((int)u - (int)e);
            if (e > 1000 && abs((int)u - (int)e) / (double)e > maxrel) maxrel = abs((int)u - (int)e) / (double)e;
        }
        if (fails > 20) return;
    }
    printf("divide: %lld samples; hardware-style differs from exact in %lld (%.3f%%), max error %d counts, max relative error %.1e\n",
           total, differ, 100.0 * differ / total, maxerr, maxrel);
}

/* colour / lighting commands: independent plain-C reference of the documented formulas, random matrices, vectors, colours, shift and clamp bits */
static void set_m(int base, const int m[3][3]) {
    gte_ctc2(base, pk(m[0][0], m[0][1])); gte_ctc2(base + 1, pk(m[0][2], m[1][0])); gte_ctc2(base + 2, pk(m[1][1], m[1][2]));
    gte_ctc2(base + 3, pk(m[2][0], m[2][1])); gte_ctc2(base + 4, (gte_u32)m[2][2]);
}
static int clamp255(long long v) { return v < 0 ? 0 : (v > 255 ? 255 : (int)v); }
static void ref_light(const int L[3][3], const int C[3][3], const int bk[3], const int vtx[3], int sf, int lm, long long mac[3], long long ir[3]) {
    long long a[3], d = sf ? 4096 : 1, s;
    int i, j;
    for (i = 0; i < 3; i++) { s = 0; for (j = 0; j < 3; j++) s += (long long)L[i][j] * vtx[j]; a[i] = sat16(floordiv(s, d), lm); }
    for (i = 0; i < 3; i++) { s = (long long)bk[i] * 4096; for (j = 0; j < 3; j++) s += (long long)C[i][j] * a[j]; mac[i] = floordiv(s, d); ir[i] = sat16(mac[i], lm); }
}
static void test_color_commands(void) {
    static const int ops[6] = { 0x1e, 0x20, 0x1b, 0x3f, 0x13, 0x16 };      /* NCS NCT NCCS NCCT NCDS NCDT */
    int trial, i, j, v;
    srand(777);
    for (trial = 0; trial < 30000; trial++) {
        int L[3][3], C[3][3], vt[3][3], bk[3], fc[3], rgb[3], ir0 = rand() % 4097, sf = rand() & 1, lm = rand() & 1, op = ops[trial % 6];
        int nv = (op == 0x20 || op == 0x3f || op == 0x16) ? 3 : 1, expect[3][3];
        long long d = sf ? 4096 : 1;
        gte_u32 cmd;
        gte_reset();
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) { L[i][j] = (rand() % 8192) - 3000; C[i][j] = (rand() % 8192) - 2000; vt[i][j] = (rand() % 8192) - 4096; }
        for (i = 0; i < 3; i++) { bk[i] = rand() % 6000; fc[i] = rand() % 8000; rgb[i] = rand() & 255; }
        set_m(GTE_C_L11L12, (const int(*)[3])L); set_m(GTE_C_LR1LR2, (const int(*)[3])C);
        gte_ctc2(GTE_C_RBK, (gte_u32)bk[0]); gte_ctc2(GTE_C_GBK, (gte_u32)bk[1]); gte_ctc2(GTE_C_BBK, (gte_u32)bk[2]);
        gte_ctc2(GTE_C_RFC, (gte_u32)fc[0]); gte_ctc2(GTE_C_GFC, (gte_u32)fc[1]); gte_ctc2(GTE_C_BFC, (gte_u32)fc[2]);
        for (v = 0; v < 3; v++) set_v(v, vt[v][0], vt[v][1], vt[v][2]);
        gte_mtc2(GTE_D_RGBC, (gte_u32)(rgb[0] | (rgb[1] << 8) | (rgb[2] << 16) | (0x2c << 24)));
        gte_mtc2(GTE_D_IR0, (gte_u32)ir0);
        for (v = 0; v < nv; v++) {
            long long mac[3], ir[3], in[3], fin[3];
            ref_light((const int(*)[3])L, (const int(*)[3])C, bk, vt[v], sf, lm, mac, ir);
            for (i = 0; i < 3; i++) {
                if (op == 0x1e || op == 0x20) fin[i] = mac[i];
                else {
                    in[i] = ((long long)rgb[i] * ir[i]) << 4;
                    if (op == 0x1b || op == 0x3f) fin[i] = floordiv(in[i], d);
                    else { long long t = sat16(floordiv(((long long)fc[i] << 12) - in[i], d), 0); fin[i] = floordiv(t * ir0 + in[i], d); }
                }
                expect[v][i] = clamp255(floordiv(fin[i], 16));
            }
        }
        cmd = 0x4a400000u | ((gte_u32)sf << 19) | ((gte_u32)lm << 10) | (gte_u32)op;
        gte_command(cmd);
        for (v = 0; v < nv; v++) {                                          /* the FIFO holds the colours oldest first: for 3 vertices RGB0 = V0 ... RGB2 = V2; for 1 only RGB2 */
            gte_u32 w = gte_mfc2(nv == 3 ? GTE_D_RGB0 + v : GTE_D_RGB2);
            for (i = 0; i < 3; i++)
                CHECK((int)((w >> (8 * i)) & 255) == expect[v][i], "op %02x trial %d vertex %d channel %d: got %d expected %d (sf %d lm %d)", op, trial, v, i, (int)((w >> (8 * i)) & 255), expect[v][i], sf, lm);
            CHECK((w >> 24) == 0x2c, "op %02x trial %d: the code byte of RGBC must be kept", op, trial);
        }
        if (fails > 20) return;
    }
    /* DPCS with IR0 = 0 gives the colour back (sf = 1); INTPL with IR0 = 0 gives IR / 16 */
    for (trial = 0; trial < 2000; trial++) {
        int rr = rand() & 255, gg = rand() & 255, bb = rand() & 255, k;
        gte_reset();
        gte_mtc2(GTE_D_RGBC, (gte_u32)(rr | (gg << 8) | (bb << 16)));
        gte_mtc2(GTE_D_IR0, 0);
        gte_command(0x4a480010u);
        k = (int)gte_mfc2(GTE_D_RGB2);
        CHECK((k & 255) == rr && ((k >> 8) & 255) == gg && ((k >> 16) & 255) == bb, "DPCS identity: %06x vs %02x%02x%02x", k & 0xffffff, bb, gg, rr);
        gte_load_ir(rr * 16, gg * 16, bb * 16);
        gte_command(0x4a480011u);
        k = (int)gte_mfc2(GTE_D_RGB2);
        CHECK((k & 255) == rr && ((k >> 8) & 255) == gg && ((k >> 16) & 255) == bb, "INTPL with IR0 = 0: %06x vs %02x%02x%02x", k & 0xffffff, bb, gg, rr);
    }
}

int main(void) {
    test_color_commands();
    test_identity_and_rotation();
    test_mvmva_random();
    test_rtps();
    test_rtpt_fifo_nclip_avsz();
    test_registers_and_flags();
    test_divide();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
