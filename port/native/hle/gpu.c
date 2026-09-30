/* A software model of the PlayStation GPU -- see gpu.h. */
#include "hle.h"
#include "gpu.h"

static unsigned rd32(hle_t* h, unsigned addr) {
    return h->p.r8(h->p.ctx, addr) | (h->p.r8(h->p.ctx, addr + 1) << 8) | (h->p.r8(h->p.ctx, addr + 2) << 16) | (h->p.r8(h->p.ctx, addr + 3) << 24);
}
static int rd16s(hle_t* h, unsigned addr) { return (short)(h->p.r8(h->p.ctx, addr) | (h->p.r8(h->p.ctx, addr + 1) << 8)); }
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int sx11(unsigned v) { return (int)(v & 0x7ffu) - (int)((v & 0x400u) << 1); }       /* 11-bit two's complement */

void gpu_reset(gpu_t* g) {
    g->clip_x1 = 0; g->clip_y1 = 0; g->clip_x2 = GPU_VRAM_W - 1; g->clip_y2 = GPU_VRAM_H - 1;
    g->ofs_x = g->ofs_y = 0;
    g->tp_x = g->tp_y = g->abr = g->tp = 0;
    g->twin_mx = g->twin_my = g->twin_ox = g->twin_oy = 0;
    g->mask_set = g->mask_check = 0;
    g->disp_x = g->disp_y = 0; g->disp_w = 256; g->disp_h = 240; g->disp_on = 1; g->disp_rgb24 = 0;
}

/* ---------------------------------------------------------------------------------------------------------------- pixels */
/* 15-bit colours are 0bSbbbbbgggggrrrrr. Semi-transparency mixes the new colour F with the pixel already there B, per 5-bit channel. */
static unsigned blend(unsigned b, unsigned f, int abr) {
    unsigned out = 0, k;
    for (k = 0; k < 15; k += 5) {
        int B = (int)((b >> k) & 31), F = (int)((f >> k) & 31), r;
        switch (abr) { case 0: r = (B + F) >> 1; break; case 1: r = B + F; break; case 2: r = B - F; break; default: r = B + (F >> 2); break; }
        if (r < 0) r = 0; else if (r > 31) r = 31;
        out |= (unsigned)r << k;
    }
    return out;
}
static void put_pixel(gpu_t* g, int x, int y, unsigned col, int semi) {
    unsigned short* p;
    if (x < g->clip_x1 || x > g->clip_x2 || y < g->clip_y1 || y > g->clip_y2) return;
    p = &g->vram[(y & 511) * GPU_VRAM_W + (x & 1023)];
    if (g->mask_check && (*p & 0x8000u)) return;
    if (semi) col = blend(*p, col, g->abr);
    *p = (unsigned short)((col & 0x7fffu) | (g->mask_set ? 0x8000u : 0));
}
static unsigned rgb555(unsigned r, unsigned g, unsigned b) { return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10); }

/* texture fetch: the texel at (u, v) of the current texture page (0 = transparent), CLUT at (clut_x * 16, clut_y) */
static unsigned fetch(gpu_t* g, int u, int v, int clut_x, int clut_y) {
    unsigned word, idx;
    u &= 255; v &= 255;
    u = (u & ~(g->twin_mx * 8)) | ((g->twin_ox & g->twin_mx) * 8);
    v = (v & ~(g->twin_my * 8)) | ((g->twin_oy & g->twin_my) * 8);
    {
        int row = (g->tp_y * 256 + v) & 511, bx = g->tp_x * 64;
        const unsigned short* line = &g->vram[row * GPU_VRAM_W];
        const unsigned short* clut = &g->vram[(clut_y & 511) * GPU_VRAM_W];
        switch (g->tp) {
        case 0: word = line[(bx + (u >> 2)) & 1023]; idx = (word >> ((u & 3) * 4)) & 15u; return clut[(clut_x * 16 + (int)idx) & 1023];
        case 1: word = line[(bx + (u >> 1)) & 1023]; idx = (word >> ((u & 1) * 8)) & 255u; return clut[(clut_x * 16 + (int)idx) & 1023];
        default: return line[(bx + u) & 1023];
        }
    }
}
/* a textured pixel: modulate the texel with the vertex colour (128 = neutral) unless the primitive is "raw", then draw it (semi-transparent if the primitive asks for it and the texel's STP bit is set) */
static void textured_pixel(gpu_t* g, int x, int y, unsigned texel, int cr, int cg, int cb, int raw, int semi) {
    unsigned col;
    if (texel == 0) return;
    if (raw) col = texel & 0x7fffu;
    else {
        int r = (int)((texel & 31) << 3) * cr >> 7, gg = (int)(((texel >> 5) & 31) << 3) * cg >> 7, b = (int)(((texel >> 10) & 31) << 3) * cb >> 7;
        col = rgb555((unsigned)(r > 255 ? 255 : r), (unsigned)(gg > 255 ? 255 : gg), (unsigned)(b > 255 ? 255 : b));
    }
    put_pixel(g, x, y, col, semi && (texel & 0x8000u));
}

/* ------------------------------------------------------------------------------------------------------- triangles */
typedef struct { int x, y, r, g, b, u, v; } vtx_t;
#define EDGE(p1, p2, px, py) (((p2).x - (p1).x) * ((py) - (p1).y) - ((p2).y - (p1).y) * ((px) - (p1).x))
static int top_left(const vtx_t* p1, const vtx_t* p2) { return (p1->y == p2->y && p2->x > p1->x) || p2->y < p1->y; }   /* clockwise (y down) triangle: top edge runs right, left edge runs up */
static void raster_tri(gpu_t* g, vtx_t a, vtx_t b, vtx_t c, int textured, int shaded, int semi, int raw, int clut_x, int clut_y) {
    int area, minx, maxx, miny, maxy, x, y, bias_a, bias_b, bias_c;
    (void)shaded;
    area = EDGE(a, b, c.x, c.y);
    if (area == 0) return;
    if (area < 0) { vtx_t t = b; b = c; c = t; area = -area; }
    minx = imin(a.x, imin(b.x, c.x)); maxx = imax(a.x, imax(b.x, c.x));
    miny = imin(a.y, imin(b.y, c.y)); maxy = imax(a.y, imax(b.y, c.y));
    if (maxx - minx > 1023 || maxy - miny > 511) return;                       /* the GPU drops polygons that are too large */
    minx = imax(minx, g->clip_x1); maxx = imin(maxx, g->clip_x2);
    miny = imax(miny, g->clip_y1); maxy = imin(maxy, g->clip_y2);
    bias_c = top_left(&a, &b) ? 0 : 1; bias_a = top_left(&b, &c) ? 0 : 1; bias_b = top_left(&c, &a) ? 0 : 1;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++) {
            int wc = EDGE(a, b, x, y), wa = EDGE(b, c, x, y), wb = EDGE(c, a, x, y);
            int r, gg, bb;
            if (wa < bias_a || wb < bias_b || wc < bias_c) continue;
            r = (wa * a.r + wb * b.r + wc * c.r) / area; gg = (wa * a.g + wb * b.g + wc * c.g) / area; bb = (wa * a.b + wb * b.b + wc * c.b) / area;
            if (textured) {
                int u = (wa * a.u + wb * b.u + wc * c.u) / area, v = (wa * a.v + wb * b.v + wc * c.v) / area;
                textured_pixel(g, x, y, fetch(g, u, v, clut_x, clut_y), r, gg, bb, raw, semi);
            } else put_pixel(g, x, y, rgb555((unsigned)r, (unsigned)gg, (unsigned)bb), semi);
        }
}

/* ---------------------------------------------------------------------------------------------- rectangles, lines */
static void draw_rect(gpu_t* g, int x, int y, int w, int h, int textured, int raw, int semi, int cr, int cg, int cb, int u0, int v0, int clut_x, int clut_y) {
    int xx, yy;
    x += g->ofs_x; y += g->ofs_y;
    for (yy = 0; yy < h; yy++) {
        if (y + yy < g->clip_y1 || y + yy > g->clip_y2) continue;
        for (xx = 0; xx < w; xx++) {
            if (x + xx < g->clip_x1 || x + xx > g->clip_x2) continue;
            if (textured) textured_pixel(g, x + xx, y + yy, fetch(g, u0 + xx, v0 + yy, clut_x, clut_y), cr, cg, cb, raw, semi);
            else put_pixel(g, x + xx, y + yy, rgb555((unsigned)cr, (unsigned)cg, (unsigned)cb), semi);
        }
    }
}
static int iabs(int v) { return v < 0 ? -v : v; }
static void draw_line(gpu_t* g, vtx_t a, vtx_t b, int shaded, int semi) {
    int dx = b.x - a.x, dy = b.y - a.y, n = imax(iabs(dx), iabs(dy)), i;
    if (iabs(dx) > 1023 || iabs(dy) > 511) return;
    (void)shaded;
    if (n == 0) { put_pixel(g, a.x, a.y, rgb555((unsigned)a.r, (unsigned)a.g, (unsigned)a.b), semi); return; }
    for (i = 0; i <= n; i++) {
        int x = a.x + (dx * i + (dx < 0 ? -n / 2 : n / 2)) / n, y = a.y + (dy * i + (dy < 0 ? -n / 2 : n / 2)) / n;
        int r = a.r + (b.r - a.r) * i / n, gg = a.g + (b.g - a.g) * i / n, bb = a.b + (b.b - a.b) * i / n;
        put_pixel(g, x, y, rgb555((unsigned)r, (unsigned)gg, (unsigned)bb), semi);
    }
}

static void fill_rect(gpu_t* g, unsigned xy, unsigned wh, unsigned color24) {                                    /* GP0(02h): ignores the drawing area */
    int x = (int)(xy & 0x3f0u), y = (int)((xy >> 16) & 0x1ffu), w = (int)(((wh & 0x3ffu) + 0xfu) & ~0xfu), h = (int)((wh >> 16) & 0x1ffu), xx, yy;
    unsigned col = rgb555(color24 & 255u, (color24 >> 8) & 255u, (color24 >> 16) & 255u);
    for (yy = 0; yy < h; yy++) for (xx = 0; xx < w; xx++) g->vram[((y + yy) & 511) * GPU_VRAM_W + ((x + xx) & 1023)] = (unsigned short)col;
}

/* ------------------------------------------------------------------------------------------------ GP0 command stream */
static void set_tpage(gpu_t* g, unsigned v) { g->tp_x = (int)(v & 15); g->tp_y = (int)((v >> 4) & 1); g->abr = (int)((v >> 5) & 3); g->tp = (int)((v >> 7) & 3); if (g->tp == 3) g->tp = 2; }
/* one command of the stream w[0..n-1] (n words are available); returns the number of words it used (at least 1) */
static unsigned gp0_command(gpu_t* g, const unsigned* w, unsigned n) {
    unsigned cmd = w[0] >> 24, code = cmd & 0xe0u, used = 1;
    if (code == 0x20) {                                                         /* polygon */
        int quad = (cmd >> 3) & 1, textured = (cmd >> 2) & 1, shaded = (cmd >> 4) & 1, semi = (cmd >> 1) & 1, raw = cmd & 1, nv = quad ? 4 : 3, i, clut_x = 0, clut_y = 0;
        vtx_t v[4];
        unsigned k = 1;                                                         /* word 0 = command + colour of vertex 0; then per vertex: [colour (shaded, vertex > 0)] xy [uv] */
        for (i = 0; i < nv; i++) {
            unsigned c = w[0];
            if (i > 0 && shaded) { if (k >= n) return n; c = w[k++]; }
            v[i].r = (int)(c & 255u); v[i].g = (int)((c >> 8) & 255u); v[i].b = (int)((c >> 16) & 255u);
            if (k >= n) return n;
            v[i].x = sx11(w[k] & 0xffffu) + g->ofs_x; v[i].y = sx11((w[k] >> 16) & 0xffffu) + g->ofs_y; k++;
            if (textured) {
                unsigned t;
                if (k >= n) return n;
                t = w[k++];
                v[i].u = (int)(t & 255u); v[i].v = (int)((t >> 8) & 255u);
                if (i == 0) { clut_x = (int)((t >> 16) & 63u); clut_y = (int)((t >> 22) & 511u); }
                if (i == 1) set_tpage(g, t >> 16);
            }
        }
        used = k;
        g->n_prims++; g->n_tris += quad ? 2u : 1u;
        raster_tri(g, v[0], v[1], v[2], textured, shaded, semi, raw, clut_x, clut_y);
        if (quad) raster_tri(g, v[1], v[2], v[3], textured, shaded, semi, raw, clut_x, clut_y);
        return used;
    }
    if (code == 0x40) {                                                         /* line / polyline */
        int shaded = (cmd >> 4) & 1, poly = (cmd >> 3) & 1, semi = (cmd >> 1) & 1;
        vtx_t prev, cur;
        unsigned k = 1, col = w[0];
        prev.r = (int)(col & 255u); prev.g = (int)((col >> 8) & 255u); prev.b = (int)((col >> 16) & 255u);
        if (k >= n) return n;
        prev.x = sx11(w[k] & 0xffffu) + g->ofs_x; prev.y = sx11((w[k] >> 16) & 0xffffu) + g->ofs_y; k++;
        for (;;) {
            unsigned c = col;
            if (shaded) { if (k >= n) return n; c = w[k++]; }
            if (k >= n) return n;
            cur.r = (int)(c & 255u); cur.g = (int)((c >> 8) & 255u); cur.b = (int)((c >> 16) & 255u);
            if (!shaded) { cur.r = prev.r; cur.g = prev.g; cur.b = prev.b; }
            cur.x = sx11(w[k] & 0xffffu) + g->ofs_x; cur.y = sx11((w[k] >> 16) & 0xffffu) + g->ofs_y; k++;
            draw_line(g, prev, cur, shaded, semi);
            g->n_lines++;
            prev = cur;
            if (!poly) break;
            if (k < n && (w[k] & 0xf000f000u) == 0x50005000u) { k++; break; }
            if (k >= n) break;
        }
        return k;
    }
    if (code == 0x60) {                                                         /* rectangle */
        int textured = (cmd >> 2) & 1, semi = (cmd >> 1) & 1, raw = cmd & 1, size = (int)((cmd >> 3) & 3), wd, ht, cr = (int)(w[0] & 255u), cg = (int)((w[0] >> 8) & 255u), cb = (int)((w[0] >> 16) & 255u);
        unsigned k = 1;
        int x, y, u = 0, vv = 0, clut_x = 0, clut_y = 0;
        if (k >= n) return n;
        x = sx11(w[k] & 0xffffu); y = sx11((w[k] >> 16) & 0xffffu); k++;
        if (textured) { unsigned t; if (k >= n) return n; t = w[k++]; u = (int)(t & 255u); vv = (int)((t >> 8) & 255u); clut_x = (int)((t >> 16) & 63u); clut_y = (int)((t >> 22) & 511u); }
        if (size == 0) { if (k >= n) return n; wd = (int)(w[k] & 0x3ffu); ht = (int)((w[k] >> 16) & 0x1ffu); k++; }
        else { wd = ht = size == 1 ? 1 : size == 2 ? 8 : 16; }
        g->n_prims++; g->n_rects++;
        draw_rect(g, x, y, wd, ht, textured, raw, semi, cr, cg, cb, u, vv, clut_x, clut_y);
        return k;
    }
    switch (cmd) {
    case 0x00: case 0x01: return 1;
    case 0x02: if (n >= 3) { fill_rect(g, w[1], w[2], w[0] & 0xffffffu); return 3; } return n;
    case 0x80: {                                                                /* VRAM -> VRAM */
        if (n >= 4) {
            int sx = (int)(w[1] & 0x3ffu), sy = (int)((w[1] >> 16) & 0x1ffu), dx = (int)(w[2] & 0x3ffu), dy = (int)((w[2] >> 16) & 0x1ffu), ww = (int)(w[3] & 0x3ffu), hh = (int)((w[3] >> 16) & 0x1ffu), xx, yy;
            if (!ww) ww = 1024; if (!hh) hh = 512;
            for (yy = 0; yy < hh; yy++) for (xx = 0; xx < ww; xx++) g->vram[((dy + yy) & 511) * GPU_VRAM_W + ((dx + xx) & 1023)] = g->vram[((sy + yy) & 511) * GPU_VRAM_W + ((sx + xx) & 1023)];
            return 4;
        }
        return n;
    }
    case 0xa0: { if (n >= 3) { unsigned ww = w[2] & 0xffffu, hh = w[2] >> 16, words = (ww * hh + 1) / 2; return 3 + (words < n - 3 ? words : n - 3); } return n; }   /* CPU -> VRAM inside a packet: not used by the game; skipped */
    case 0xc0: return n >= 3 ? 3 : n;
    case 0xe1: set_tpage(g, w[0] & 0x7ffu); return 1;
    case 0xe2: g->twin_mx = (int)(w[0] & 31u); g->twin_my = (int)((w[0] >> 5) & 31u); g->twin_ox = (int)((w[0] >> 10) & 31u); g->twin_oy = (int)((w[0] >> 15) & 31u); return 1;
    case 0xe3: g->clip_x1 = (int)(w[0] & 0x3ffu); g->clip_y1 = (int)((w[0] >> 10) & 0x3ffu); if (g->clip_y1 > 511) g->clip_y1 = 511; return 1;
    case 0xe4: g->clip_x2 = (int)(w[0] & 0x3ffu); g->clip_y2 = (int)((w[0] >> 10) & 0x3ffu); if (g->clip_y2 > 511) g->clip_y2 = 511; return 1;
    case 0xe5: g->ofs_x = sx11(w[0] & 0x7ffu); g->ofs_y = sx11((w[0] >> 11) & 0x7ffu); return 1;
    case 0xe6: g->mask_set = (int)(w[0] & 1u); g->mask_check = (int)((w[0] >> 1) & 1u); return 1;
    default: g->n_unknown++; return 1;
    }
}
/* one packet of an ordering table: `len` command words after the tag */
static void gp0_packet(hle_t* h, unsigned addr, unsigned len) {
    unsigned w[64], k, used;
    gpu_t* g = h->gpu;
    if (len > 63) len = 63;
    for (k = 0; k < len; k++) w[k] = rd32(h, addr + 4 + 4 * k);
    for (k = 0; k < len; k += used) used = gp0_command(g, w + k, len - k);
}
static void draw_otag(hle_t* h, unsigned ot) {
    unsigned addr = ot, guard = 0;
    while (guard++ < 200000) {
        unsigned tag, len;
        if (h->p.valid ? !h->p.valid(h->p.ctx, addr) : !(addr >= 0x80000000u && addr < 0x80200000u)) return;
        tag = rd32(h, addr); len = tag >> 24;
        if (len) gp0_packet(h, addr, len);
        if ((tag & 0x00ffffffu) == 0x00ffffffu) return;
        addr = 0x80000000u | (tag & 0x00ffffffu);
    }
}

/* ------------------------------------------------------------------------------------------------ SDK entry points */
static void load_image(hle_t* h, unsigned rect, unsigned data) {
    gpu_t* g = h->gpu;
    int x = rd16s(h, rect), y = rd16s(h, rect + 2), w = rd16s(h, rect + 4), hh = rd16s(h, rect + 6), xx, yy;
    unsigned a = data;
    for (yy = 0; yy < hh; yy++)
        for (xx = 0; xx < w; xx++, a += 2) g->vram[((y + yy) & 511) * GPU_VRAM_W + ((x + xx) & 1023)] = (unsigned short)(h->p.r8(h->p.ctx, a) | (h->p.r8(h->p.ctx, a + 1) << 8));
}
static void store_image(hle_t* h, unsigned rect, unsigned data) {
    gpu_t* g = h->gpu;
    int x = rd16s(h, rect), y = rd16s(h, rect + 2), w = rd16s(h, rect + 4), hh = rd16s(h, rect + 6), xx, yy;
    unsigned a = data;
    for (yy = 0; yy < hh; yy++)
        for (xx = 0; xx < w; xx++, a += 2) {
            unsigned short c = g->vram[((y + yy) & 511) * GPU_VRAM_W + ((x + xx) & 1023)];
            unsigned char b[2];
            b[0] = (unsigned char)(c & 255u); b[1] = (unsigned char)(c >> 8);
            h->p.write_bytes(h->p.ctx, a, b, 2);
        }
}
int gpu_hle_call(hle_t* h, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret) {
    gpu_t* g = h->gpu;
    (void)a3;
    *ret = 0;
    if (streq(n, "DrawOTag")) { draw_otag(h, a0); return 1; }
    if (streq(n, "DrawPrim")) { unsigned tag = rd32(h, a0); if (tag >> 24) gp0_packet(h, a0, tag >> 24); return 1; }
    if (streq(n, "LoadImage")) { load_image(h, a0, a1); return 1; }
    if (streq(n, "StoreImage")) { store_image(h, a0, a1); return 1; }
    if (streq(n, "MoveImage")) {
        int sx = rd16s(h, a0), sy = rd16s(h, a0 + 2), w = rd16s(h, a0 + 4), hh = rd16s(h, a0 + 6), xx, yy;
        unsigned short tmp[64];
        (void)tmp;
        for (yy = 0; yy < hh; yy++) for (xx = 0; xx < w; xx++) g->vram[(((int)a2 + yy) & 511) * GPU_VRAM_W + (((int)a1 + xx) & 1023)] = g->vram[((sy + yy) & 511) * GPU_VRAM_W + ((sx + xx) & 1023)];
        return 1;
    }
    if (streq(n, "ClearImage")) { fill_rect(g, (unsigned)(rd16s(h, a0) & 0xffff) | ((unsigned)rd16s(h, a0 + 2) << 16), (unsigned)(rd16s(h, a0 + 4) & 0xffff) | ((unsigned)rd16s(h, a0 + 6) << 16), (a1 & 255u) | ((a2 & 255u) << 8) | ((a3 & 255u) << 16)); return 1; }
    if (streq(n, "PutDispEnv")) {
        g->disp_x = rd16s(h, a0); g->disp_y = rd16s(h, a0 + 2); g->disp_w = rd16s(h, a0 + 4); g->disp_h = rd16s(h, a0 + 6);
        g->disp_rgb24 = h->p.r8(h->p.ctx, a0 + 17) & 1;
        return 1;
    }
    if (streq(n, "PutDrawEnv")) {                                               /* DRAWENV: clip, ofs, tw, tpage, dtd, dfe, isbg, r0 g0 b0 */
        int cx = rd16s(h, a0), cy = rd16s(h, a0 + 2), cw = rd16s(h, a0 + 4), ch = rd16s(h, a0 + 6), twx = rd16s(h, a0 + 12), twy = rd16s(h, a0 + 14), tww = rd16s(h, a0 + 16), twh = rd16s(h, a0 + 18);
        g->clip_x1 = cx; g->clip_y1 = cy; g->clip_x2 = cx + cw - 1; g->clip_y2 = cy + ch - 1;
        g->ofs_x = rd16s(h, a0 + 8); g->ofs_y = rd16s(h, a0 + 10);
        if (tww || twh) { g->twin_mx = ((-tww) & 0xff) >> 3; g->twin_my = ((-twh) & 0xff) >> 3; g->twin_ox = twx >> 3; g->twin_oy = twy >> 3; }
        else g->twin_mx = g->twin_my = g->twin_ox = g->twin_oy = 0;
        set_tpage(g, (unsigned)(h->p.r8(h->p.ctx, a0 + 20) | (h->p.r8(h->p.ctx, a0 + 21) << 8)) & 0x1ffu);
        if (h->p.r8(h->p.ctx, a0 + 24)) {                                       /* isbg: clear the clip area (ignores the offset) */
            unsigned col = h->p.r8(h->p.ctx, a0 + 25) | (h->p.r8(h->p.ctx, a0 + 26) << 8) | (h->p.r8(h->p.ctx, a0 + 27) << 16);
            fill_rect(g, (unsigned)(cx & 0xffff) | ((unsigned)cy << 16), (unsigned)(cw & 0xffff) | ((unsigned)ch << 16), col);
        }
        return 1;
    }
    if (streq(n, "SetDispMask")) { g->disp_on = a0 != 0; return 1; }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------- output */
void gpu_display_rgb(const gpu_t* g, unsigned char* out, int* w, int* h) {
    int x, y, ww = g->disp_w > 0 ? g->disp_w : 256, hh = g->disp_h > 0 ? g->disp_h : 240;
    if (ww > GPU_VRAM_W) ww = GPU_VRAM_W;
    if (hh > GPU_VRAM_H) hh = GPU_VRAM_H;
    *w = ww; *h = hh;
    for (y = 0; y < hh; y++)
        for (x = 0; x < ww; x++) {
            unsigned c = g->disp_on ? g->vram[((g->disp_y + y) & 511) * GPU_VRAM_W + ((g->disp_x + x) & 1023)] : 0, r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
            unsigned char* p = out + 3 * (y * ww + x);
            p[0] = (unsigned char)((r << 3) | (r >> 2)); p[1] = (unsigned char)((gg << 3) | (gg >> 2)); p[2] = (unsigned char)((b << 3) | (b >> 2));
        }
}
unsigned gpu_hash_vram(const gpu_t* g) {
    unsigned x = 2166136261u, i;
    for (i = 0; i < GPU_VRAM_W * GPU_VRAM_H; i++) x = (x ^ g->vram[i]) * 16777619u;
    return x;
}
unsigned gpu_hash_display(const gpu_t* g) {
    unsigned x = 2166136261u;
    int xx, y;
    for (y = 0; y < g->disp_h; y++) for (xx = 0; xx < g->disp_w; xx++) x = (x ^ g->vram[((g->disp_y + y) & 511) * GPU_VRAM_W + ((g->disp_x + xx) & 1023)]) * 16777619u;
    return x;
}
