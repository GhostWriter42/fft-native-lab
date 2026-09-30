/* Native libgte test: the native replacements (libgte_native.c on the software GTE) and the reconstructed plain-C
 * functions (rsin/rcos/ratan2/csqrt) checked against independent references. Freestanding -m32 + the RAM image, because
 * the sine/sqrt tables are the game's own data. */
#include "psx/libgte.h"
#include "gte.h"

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char* s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void outnum(long v) {
    char buf[24]; int i = 23, neg = v < 0; buf[23] = 0;
    if (neg) v = -v;
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    out(buf + i);
}
struct old_mmap_args { long addr, len, prot, flags, fd, off; };
static int map_ram(void) { struct old_mmap_args a = { 0x80000000, 0x200000, 3, 0x32, -1, 0 }; return sys3(90, (long)&a, 0, 0) == (long)0x80000000; }
static long load_file(const char* path, unsigned addr) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(addr + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}

/* ---- tiny xorshift RNG for randomized tests ---- */
static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rrange(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static int checks, fails;
static void check(int ok, const char* what, long a, long b) {
    checks++;
    if (!ok) { fails++; if (fails <= 25) { out("FAIL "); out(what); out(": "); outnum(a); out(" vs "); outnum(b); out("\n"); } }
}
static long labs_(long v) { return v < 0 ? -v : v; }

/* ---- double-precision references without libm ---- */
static double dabs(double x) { return x < 0 ? -x : x; }
static double ref_sin(double x) {          /* x in radians, reduced to [-pi, pi], Taylor to x^17 */
    const double PI = 3.14159265358979323846;
    double t, s;
    int k;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    t = x; s = x;
    for (k = 1; k < 9; k++) { t *= -x * x / ((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
static double ref_sqrt(double v) { double x = v > 1 ? v : 1; int i; if (v <= 0) return 0; for (i = 0; i < 60; i++) x = 0.5 * (x + v / x); return x; }
static double ref_atan(double z) {         /* |z| <= 1 via series after argument reduction */
    double a = dabs(z), r;
    int inv = a > 1;
    const double PI = 3.14159265358979323846;
    double t, s, x2;
    int k;
    if (inv) a = 1.0 / a;
    /* reduce further: atan(a) = 2*atan(a / (1 + sqrt(1 + a^2))) */
    a = a / (1.0 + ref_sqrt(1.0 + a * a));
    a = a / (1.0 + ref_sqrt(1.0 + a * a));
    x2 = a * a; t = a; s = a;
    for (k = 1; k < 12; k++) { t *= -x2; s += t / (2 * k + 1); }
    r = 4.0 * s;
    if (inv) r = PI / 2 - r;
    return z < 0 ? -r : r;
}
static double ref_atan2(double y, double x) {
    const double PI = 3.14159265358979323846;
    if (x > 0) return ref_atan(y / x);
    if (x < 0) return y >= 0 ? ref_atan(y / x) + PI : ref_atan(y / x) - PI;
    return y > 0 ? PI / 2 : (y < 0 ? -PI / 2 : 0);
}

int main_test(void) {
    int i, j, k, n;
    const double PI = 3.14159265358979323846;

    /* --- rsin / rcos vs the true functions (12-bit angle, 4096 = 1.0) --- */
    for (i = -4096; i <= 8192; i += 7) {
        double s = ref_sin(2 * PI * i / 4096.0) * 4096.0, c = ref_sin(2 * PI * i / 4096.0 + PI / 2) * 4096.0;
        check(dabs(rsin(i) - s) <= 2.0, "rsin", rsin(i), (long)s);
        check(dabs(rcos(i) - c) <= 2.0, "rcos", rcos(i), (long)c);
    }
    /* --- ratan2: result in 12-bit turns (4096 = full circle) --- */
    for (n = 0; n < 4000; n++) {
        long y = rrange(-30000, 30000), x = rrange(-30000, 30000);
        double a;
        long got, want;
        if (!x && !y) continue;
        a = ref_atan2((double)y, (double)x) * 4096.0 / (2 * PI);
        want = (long)(a < 0 ? a - 0.5 : a + 0.5);
        got = ratan2(y, x);
        check(labs_(got - want) <= 2 || labs_(labs_(got - want) - 4096) <= 2, "ratan2", got, want);
    }
    /* --- csqrt: square root of a 20.12 fixed-point value, returned as 20.12 (so csqrt(v) = sqrt(v * 4096)) --- */
    for (n = 0; n < 2000; n++) {
        long v = rrange(1000, 100000000);
        long got = csqrt(v);
        double want = ref_sqrt((double)v * 4096.0);
        check(dabs((double)got - want) <= want * 0.01 + 1.0, "csqrt", got, (long)want);
    }
    check(csqrt(0) == 0, "csqrt(0)", csqrt(0), 0);
    /* --- SquareRoot0 / SquareRoot12 --- */
    for (n = 0; n < 4000; n++) {
        long v = rrange(1, 2000000000);
        double want0 = ref_sqrt((double)v);
        double want12 = ref_sqrt((double)v * 4096.0);      /* input 20.12 -> output 20.12 */
        long g0 = SquareRoot0(v);
        long g12 = SquareRoot12((s32)v);
        check(dabs((double)g0 - want0) <= want0 * 0.02 + 2.0, "SquareRoot0", g0, (long)want0);
        check(dabs((double)g12 - want12) <= want12 * 0.02 + 8.0, "SquareRoot12", g12, (long)want12);
    }
    check(SquareRoot0(0) == 0, "SquareRoot0(0)", SquareRoot0(0), 0);

    /* --- RotMatrix --- */
    {
        SVECTOR a; MATRIX m;
        a.vx = 0; a.vy = 0; a.vz = 0; RotMatrix(&a, &m);
        check(m.m[0][0] == 4096 && m.m[1][1] == 4096 && m.m[2][2] == 4096 && !m.m[0][1] && !m.m[0][2] && !m.m[1][0] && !m.m[1][2] && !m.m[2][0] && !m.m[2][1],
              "RotMatrix identity", m.m[0][0], 4096);
        a.vx = 0; a.vy = 0; a.vz = 1024; RotMatrix(&a, &m);         /* +90 degrees about Z */
        check(labs_(m.m[0][0]) <= 2 && labs_(m.m[1][1]) <= 2 && labs_(m.m[0][1] + 4096) <= 2 && labs_(m.m[1][0] - 4096) <= 2 && m.m[2][2] == 4096,
              "RotMatrix Rz90", m.m[0][1], -4096);
        for (n = 0; n < 3000; n++) {
            long dot;
            a.vx = rrange(-4096, 4096); a.vy = rrange(-4096, 4096); a.vz = rrange(-4096, 4096);
            RotMatrix(&a, &m);
            /* rows must be orthonormal to ~1e-3: |row|^2 ~ 4096^2, row_i . row_j ~ 0 */
            for (i = 0; i < 3; i++) {
                long nn = (long)m.m[i][0] * m.m[i][0] + (long)m.m[i][1] * m.m[i][1] + (long)m.m[i][2] * m.m[i][2];
                check(labs_(nn - 4096L * 4096L) <= 4096L * 40, "RotMatrix row norm", nn, 4096L * 4096L);
                for (j = i + 1; j < 3; j++) {
                    dot = (long)m.m[i][0] * m.m[j][0] + (long)m.m[i][1] * m.m[j][1] + (long)m.m[i][2] * m.m[j][2];
                    check(labs_(dot) <= 4096L * 40, "RotMatrix row dot", dot, 0);
                }
            }
        }
    }

    /* --- MulMatrix family vs exact integer reference --- */
    for (n = 0; n < 2000; n++) {
        MATRIX l, r, o, orig_l;
        long want[3][3];
        int p, q, t;
        for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) { l.m[p][q] = (s16)rrange(-4096, 4096); r.m[p][q] = (s16)rrange(-4096, 4096); }
        for (p = 0; p < 3; p++) { l.t[p] = rrange(-100000, 100000); r.t[p] = rrange(-100000, 100000); }
        orig_l = l;
        for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) {
            long long s = 0;
            for (t = 0; t < 3; t++) s += (long long)l.m[p][t] * r.m[t][q];
            s >>= 12;
            want[p][q] = s < -32768 ? -32768 : (s > 32767 ? 32767 : (long)s);
        }
        MulMatrix0(&l, &r, &o);
        for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) check(o.m[p][q] == want[p][q], "MulMatrix0", o.m[p][q], want[p][q]);
        {
            MATRIX l2 = orig_l, r2 = r;
            MulMatrix(&l2, &r2);
            for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) check(l2.m[p][q] == want[p][q], "MulMatrix", l2.m[p][q], want[p][q]);
            check(l2.t[0] == orig_l.t[0] && l2.t[1] == orig_l.t[1] && l2.t[2] == orig_l.t[2], "MulMatrix keeps translation", l2.t[0], orig_l.t[0]);
            l2 = orig_l; r2 = r;
            MulMatrix2(&l2, &r2);
            for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) check(r2.m[p][q] == want[p][q], "MulMatrix2", r2.m[p][q], want[p][q]);
            check(r2.t[0] == r.t[0], "MulMatrix2 keeps right translation", r2.t[0], r.t[0]);
        }
    }

    /* --- ApplyMatrixLV vs exact (M * v) >> 12 --- */
    for (n = 0; n < 3000; n++) {
        MATRIX m; VECTOR v, o; int p, t;
        for (p = 0; p < 3; p++) for (t = 0; t < 3; t++) m.m[p][t] = (s16)rrange(-4096, 4096);
        v.vx = rrange(-1000000000, 1000000000) / (1 + (int)(rnd() % 4)); v.vy = rrange(-100000, 100000); v.vz = rrange(-30000000, 30000000);
        ApplyMatrixLV(&m, &v, &o);
        {
            long long vv[3] = { v.vx, v.vy, v.vz };
            long long ref[3];
            for (p = 0; p < 3; p++) {
                long long s = 0;
                for (t = 0; t < 3; t++) s += (long long)m.m[p][t] * vv[t];
                ref[p] = s >> 12;
            }
            check(labs_(o.vx - (long)ref[0]) <= 2, "ApplyMatrixLV x", o.vx, (long)ref[0]);
            check(labs_(o.vy - (long)ref[1]) <= 2, "ApplyMatrixLV y", o.vy, (long)ref[1]);
            check(labs_(o.vz - (long)ref[2]) <= 2, "ApplyMatrixLV z", o.vz, (long)ref[2]);
        }
    }

    /* --- VectorNormal: |result| ~ 4096, direction preserved --- */
    for (n = 0; n < 3000; n++) {
        VECTOR v, o;
        double len;
        v.vx = rrange(-20000, 20000); v.vy = rrange(-20000, 20000); v.vz = rrange(-20000, 20000);
        if (labs_(v.vx) + labs_(v.vy) + labs_(v.vz) < 300) continue;
        VectorNormal(&v, &o);
        len = ref_sqrt((double)o.vx * o.vx + (double)o.vy * o.vy + (double)o.vz * o.vz);
        check(dabs(len - 4096.0) <= 4096.0 * 0.02, "VectorNormal length", (long)len, 4096);
        {   /* cross product of input and output ~ 0 relative to magnitudes */
            double ix = v.vx, iy = v.vy, iz = v.vz, ox = o.vx, oy = o.vy, oz = o.vz;
            double cx = iy * oz - iz * oy, cy = iz * ox - ix * oz, cz = ix * oy - iy * ox;
            double c = ref_sqrt(cx * cx + cy * cy + cz * cz), il = ref_sqrt(ix * ix + iy * iy + iz * iz);
            check(c <= il * len * 0.02, "VectorNormal direction", (long)(c * 1000 / (il * len)), 20);
        }
    }

    /* --- Scale --- */
    for (n = 0; n < 1000; n++) {
        MATRIX m, a, b; VECTOR sc; int p, q;
        for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) m.m[p][q] = (s16)rrange(-4096, 4096);
        sc.vx = rrange(-8192, 8192); sc.vy = rrange(-8192, 8192); sc.vz = rrange(-8192, 8192);
        a = m; ScaleMatrix(&a, &sc);
        b = m; ScaleMatrixL(&b, &sc);
        for (p = 0; p < 3; p++) for (q = 0; q < 3; q++) {
            long sq = q == 0 ? sc.vx : (q == 1 ? sc.vy : sc.vz), sp = p == 0 ? sc.vx : (p == 1 ? sc.vy : sc.vz);
            long wa = (long)(s16)((long)m.m[p][q] * sq >> 12), wb = (long)(s16)((long)m.m[p][q] * sp >> 12);
            check(a.m[p][q] == wa, "ScaleMatrix column", a.m[p][q], wa);
            check(b.m[p][q] == wb, "ScaleMatrixL row", b.m[p][q], wb);
        }
    }

    /* --- geometry pipeline through the API: RotTransPers against a float projection --- */
    for (n = 0; n < 2000; n++) {
        MATRIX cam; SVECTOR v; s32 sxy, p, flag; s32 depth;
        double fx, fy;
        int H = rrange(300, 900), x = rrange(-300, 300), y = rrange(-300, 300), z = rrange(600, 3000), sx, sy;
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) cam.m[i][j] = (s16)(i == j ? 4096 : 0);
        cam.t[0] = 0; cam.t[1] = 0; cam.t[2] = 0;
        SetRotMatrix(&cam); SetTransMatrix(&cam);
        SetGeomOffset(160, 120); SetGeomScreen(H);
        v.vx = (s16)x; v.vy = (s16)y; v.vz = (s16)z;
        depth = RotTransPers(&v, &sxy, &p, &flag);
        sx = (s16)(sxy & 0xffff); sy = (s16)((unsigned)sxy >> 16);
        fx = 160 + (double)H * x / z; fy = 120 + (double)H * y / z;
        check(labs_(sx - (long)fx) <= 1 && labs_(sy - (long)fy) <= 1, "RotTransPers", sx, (long)fx);
        check(depth == (z >> 2), "RotTransPers depth", depth, z >> 2);
        (void)k;
    }
    /* RotTrans against a translated + rotated vector */
    {
        MATRIX m; SVECTOR v; VECTOR o; long flag; SVECTOR r;
        r.vx = 0; r.vy = 0; r.vz = 1024; RotMatrix(&r, &m);
        m.t[0] = 100; m.t[1] = 200; m.t[2] = 300;
        SetRotMatrix(&m); SetTransMatrix(&m);
        v.vx = 1000; v.vy = 0; v.vz = 0;
        RotTrans(&v, &o, &flag);
        /* Rz(+90): (1000,0,0) -> (0,1000,0) then + t */
        check(labs_(o.vx - 100) <= 2 && labs_(o.vy - 1200) <= 2 && labs_(o.vz - 300) <= 2, "RotTrans Rz90+T", o.vx, 100);
    }
    return fails != 0;
}

void _start(void) {
    int bad;
    if (!map_ram()) { out("mmap FAILED\n"); sys3(1, 1, 0, 0); }
    load_file("/disc/SCUS_942.21", 0x8000f800);
    load_file("/disc/BATTLE.BIN", 0x80067000);
    gte_reset();
    bad = main_test();
    outnum(checks); out(" checks, "); outnum(fails); out(" failures\n");
    sys3(1, bad, 0, 0);
}
