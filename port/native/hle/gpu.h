#ifndef GPU_H
#define GPU_H
/* A software model of the PlayStation GPU: a 1024x512 x 16-bit VRAM, the drawing environment (clip area, offset, texture page / window), and a rasteriser for the
 * primitives the libgpu packets carry (flat / gouraud / textured triangles and quads, sprites and tiles, lines, VRAM fills and copies). The HLE feeds it the same calls the
 * real GPU would get (DrawOTag, LoadImage, ...), so that BOTH machines of the lockstep (the original code on the interpreter, the native build) draw pictures -- which can be
 * written out as images and compared pixel for pixel. Freestanding C. Accuracy target: a faithful, plain software renderer (no dithering, exact texture-window / CLUT / STP
 * semantics, the four semi-transparency modes), not cycle-exact hardware behaviour. The high-resolution renderer of the native port grows out of this. */

#define GPU_VRAM_W 1024
#define GPU_VRAM_H 512

typedef struct gpu {
    unsigned short vram[GPU_VRAM_W * GPU_VRAM_H];
    /* drawing environment */
    int clip_x1, clip_y1, clip_x2, clip_y2;                                     /* inclusive */
    int ofs_x, ofs_y;
    int tp_x, tp_y, abr, tp;                                                    /* texture page base (x in 64-pixel units, y in 256-pixel units), blend mode, colour depth (0 4bit, 1 8bit, 2 15bit) */
    int twin_mx, twin_my, twin_ox, twin_oy;                                     /* texture window (in 8-pixel units) */
    int mask_set, mask_check;
    /* display environment */
    int disp_x, disp_y, disp_w, disp_h, disp_on, disp_rgb24;
    /* statistics */
    unsigned n_prims, n_tris, n_rects, n_lines, n_unknown;
} gpu_t;

struct hle;
void gpu_reset(gpu_t* g);
/* The SDK calls the GPU model handles (the ones in hle.h HLE_EXPLICIT_NAMES that touch the GPU): returns 1 when `name` was handled (result in *ret). */
int gpu_hle_call(struct hle* h, const char* name, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret);
/* The current display area as 8-bit RGB (3 bytes per pixel); returns the size. out must hold 1024 * 512 * 3 bytes. */
void gpu_display_rgb(const gpu_t* g, unsigned char* out, int* w, int* h);
/* 32-bit hash of the whole VRAM / of the display area only */
unsigned gpu_hash_vram(const gpu_t* g);
unsigned gpu_hash_display(const gpu_t* g);
#endif
