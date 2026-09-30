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

int main(void) {
    test_identity_and_rotation();
    test_mvmva_random();
    test_rtps();
    test_rtpt_fifo_nclip_avsz();
    test_registers_and_flags();
    test_divide();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
