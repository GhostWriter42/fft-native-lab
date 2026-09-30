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
    return 0;
}
