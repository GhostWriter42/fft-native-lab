/* Unit test of the software GPU's rasteriser: draws a textured gouraud quad (the shape of the map wall polygons) over a synthetic texture and prints what arrives.
 * docker: gcc -m32 -O1 -I port/native/hle port/native/hle/test_gpu.c -o /tmp/test_gpu && /tmp/test_gpu */
#include <stdio.h>
#include <string.h>
#include "hle.h"
static int g_lu, g_lv;
#define GPU_TEST_HOOK(u, v) do { g_lu = (u); g_lv = (v); } while (0)
#include "gpu.c"

static gpu_t g;

int main(void) {
    unsigned w[12];
    int x, y, u, v;
    memset(&g, 0, sizeof g);
    gpu_reset(&g);
    /* texture page x=12, y=0, 4-bit: texel (u, v) = (u / 4 + v / 2) & 15, so the colour changes along BOTH axes */
    for (v = 0; v < 256; v++)
        for (u = 0; u < 256; u += 4) {
            unsigned word = 0;
            int k;
            for (k = 0; k < 4; k++) word |= (unsigned)(((u + k) / 4 + v / 2) & 15) << (4 * k);
            g.vram[v * 1024 + 12 * 64 + u / 4] = (unsigned short)word;
        }
    /* CLUT at (16, 480): entry i = distinct grey levels / colours */
    for (u = 0; u < 16; u++) g.vram[480 * 1024 + 16 + u] = (unsigned short)((u * 2) | ((u * 2) << 5) | ((31 - u * 2) << 10) | 0x0000);
    g.vram[480 * 1024 + 16] = 0x0421;
    /* the first wall polygon of the dump: (41,116 uv 208,57) (41,106 uv 208,48) (61,125 uv 227,57) (61,115 uv 227,48), clut (1,480), tpage x12 y0 4-bit, gouraud white */
    w[0] = 0x3cffffffu; w[1] = (116u << 16) | 41u; w[2] = (1u << 16) | (480u << 22) | (57u << 8) | 208u;
    w[3] = 0x00ffffffu; w[4] = (106u << 16) | 41u; w[5] = ((unsigned)12 << 16) | (48u << 8) | 208u;
    w[6] = 0x00ffffffu; w[7] = (125u << 16) | 61u; w[8] = (57u << 8) | 227u;
    w[9] = 0x00ffffffu; w[10] = (115u << 16) | 61u; w[11] = (48u << 8) | 227u;
    g.clip_x2 = 255; g.clip_y2 = 239;
    printf("used %u words\n", gp0_command(&g, w, 12));
    for (y = 104; y < 128; y++) {
        for (x = 40; x < 64; x++) {
            unsigned c = g.vram[y * 1024 + x];
            printf("%c", c ? "0123456789abcdef"[(c & 31) / 2 & 15] : '.');
        }
        printf("\n");
    }
    /* sample: draw single pixels of the triangle and print (u, v) */
    {
        int sy2;
        for (sy2 = 110; sy2 <= 124; sy2 += 2) {
            for (x = 42; x <= 60; x += 6) {
                memset(g.vram + 120 * 1024, 0, 1024 * 2);
                g_lu = g_lv = -1;
                g.clip_x1 = x; g.clip_x2 = x; g.clip_y1 = sy2; g.clip_y2 = sy2;
                gp0_command(&g, w, 12);
                printf("(%d,%d):%d,%d ", x, sy2, g_lu, g_lv);
            }
            printf("\n");
        }
    }
    /* HD canvas: a 1:1 sprite must come out as the nearest upscale of its 1x image; a polygon must cover about the same area */
    {
        static unsigned short hdbuf[2][64 * 64 * 9];
        unsigned sp[4], quad[12];
        int bad = 0, cnt1 = 0, cnthd = 0, i, j;
        memset(&g, 0, sizeof g);
        gpu_reset(&g);
        for (v = 0; v < 256; v++) for (u = 0; u < 256; u += 4) { unsigned word = 0; int k; for (k = 0; k < 4; k++) word |= (unsigned)(((u + k) / 4 + v) & 15) << (4 * k); g.vram[v * 1024 + 12 * 64 + u / 4] = (unsigned short)word; }
        for (u = 0; u < 16; u++) g.vram[480 * 1024 + 16 + u] = (unsigned short)(((u * 2) | ((u * 2) << 5) | ((31 - u * 2) << 10)) | 0x0001);
        g.hd_s = 3; g.hd_w = 64; g.hd_h = 64; g.hd[0] = hdbuf[0]; g.hd[1] = hdbuf[1];
        g.clip_x1 = 0; g.clip_y1 = 0; g.clip_x2 = 63; g.clip_y2 = 63;
        g.hd_n = 1; g.hd_fx[0] = 0; g.hd_fy[0] = 0;
        g.tp_x = 12; g.tp_y = 0; g.tp = 0;
        sp[0] = 0x64808080u; sp[1] = (8u << 16) | 8u; sp[2] = (1u << 16) | (480u << 22) | (16u << 8) | 16u; sp[3] = (16u << 16) | 16u;      /* SPRT 16x16 at (8,8), uv (16,16) */
        gp0_command(&g, sp, 4);
        for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
            unsigned short c1 = g.vram[y * 1024 + x] & 0x7fff;
            if (c1) cnt1++;
            for (j = 0; j < 3; j++) for (i = 0; i < 3; i++) { unsigned short ch = hdbuf[0][(y * 3 + j) * 192 + x * 3 + i]; if (ch) cnthd++; if (ch != c1) bad++; }
        }
        printf("sprite: %d pixels at 1x, %d HD pixels, %d mismatches against the nearest upscale\n", cnt1, cnthd, bad);
        memset(hdbuf, 0, sizeof hdbuf);
        memset(g.vram, 0, 1024 * 64 * 2);
        quad[0] = 0x3cffffffu; quad[1] = (10u << 16) | 10u; quad[2] = (1u << 16) | (480u << 22) | (0u << 8) | 0u;
        quad[3] = 0xffffffu; quad[4] = (10u << 16) | 40u; quad[5] = ((unsigned)12 << 16) | (30u << 8) | 0u;
        quad[6] = 0xffffffu; quad[7] = (30u << 16) | 10u; quad[8] = (0u << 8) | 30u;
        quad[9] = 0xffffffu; quad[10] = (30u << 16) | 40u; quad[11] = (30u << 8) | 30u;
        gp0_command(&g, quad, 12);
        cnt1 = cnthd = 0;
        for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) { if (g.vram[y * 1024 + x] & 0x7fff) cnt1++; for (j = 0; j < 3; j++) for (i = 0; i < 3; i++) if (hdbuf[0][(y * 3 + j) * 192 + x * 3 + i]) cnthd++; }
        printf("quad 30x20: %d pixels at 1x, %d HD pixels (expect about 9x)\n", cnt1, cnthd);
    }
    return 0;
}
