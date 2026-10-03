"""gl_renderer.py -- draws the native game's GPU command trace (hle/gpu.c "gltrace") with OpenGL.

The container (the game plus its software GPU model, which stays the oracle) sends, per frame, every triangle already parsed into vertices with absolute VRAM coordinates
(1/16 pixel), the VRAM transfers (uploads with their pixels, copies, fills) and the display settings. This class replays them on the graphics card:

* VRAM as a 1024x512 R16UI texture (what the texture fetches and CLUT lookups read: 4-bit, 8-bit and 15-bit pages, texture window, the transparent texel 0) kept by the
  uploads / copies / fills only -- a texture that the game *renders* and then samples is not reproduced (the oracle shows whether the game ever does that);
* the framebuffers as one RGBA8 texture of 1024*S x 512*S pixels (the HD "VRAM"): triangles are rasterised at S times the resolution, the display area is shown from it;
* the PS1 semi-transparency modes (0.5B+0.5F, B+F, B-F, B+0.25F) with GL blending; a textured semi-transparent primitive is drawn in two passes (texels with the STP bit clear are
  opaque, the others blended). Gouraud shading, modulation (128 = neutral) and the 5-bit colour quantisation follow the software model; there is no dithering.

Geometry is shifted by half a pixel: the software model samples at integer coordinates, GL at pixel centres.
"""
import struct

import moderngl
import numpy as np

VERT = np.dtype([('x', '<i4'), ('y', '<i4'), ('u', '<i2'), ('v', '<i2'), ('rgbf', 'u1', (4,)), ('clut', '<u2'), ('tpage', '<u2'), ('twin', 'u1', (4,))])
assert VERT.itemsize == 24
MAGIC = 0x31544C47          # 'GLT1'
SRC1_COLOR, SRC1_ALPHA = 0x88F9, 0x8589      # GL enums of dual-source blending (moderngl does not name them)

VS = """
#version 330
in ivec2 in_pos; in ivec2 in_uv; in uvec4 in_rgbf; in uvec2 in_ct; in uvec4 in_twin;
out vec3 v_col; out vec2 v_uv;
flat out uint v_flags; flat out uint v_clut; flat out uint v_tpage; flat out uvec4 v_twin;
void main() {
    vec2 p = vec2(in_pos) / 16.0 + 0.5;
    gl_Position = vec4(p.x / 1024.0 * 2.0 - 1.0, p.y / 512.0 * 2.0 - 1.0, 0.0, 1.0);
    v_col = vec3(in_rgbf.rgb); v_uv = vec2(in_uv); v_flags = in_rgbf.a; v_clut = in_ct.x; v_tpage = in_ct.y; v_twin = in_twin;
}
"""

FS = """
#version 330
uniform usampler2D vram;
uniform int u_pass;        // 0: every pixel; 1: the pixels that are NOT blended; 2: the blended ones (semi-transparent primitive, texel STP bit set or untextured)
uniform int u_abr;         // semi-transparency mode of the draw (0..3); the blend factors of each pixel go to the second output (dual-source blending)
in vec3 v_col; in vec2 v_uv;
flat in uint v_flags; flat in uint v_clut; flat in uint v_tpage; flat in uvec4 v_twin;
layout(location = 0, index = 0) out vec4 f_color;
layout(location = 0, index = 1) out vec4 f_blend;   // .rgb = source factor, .a = destination factor
uint tex16(int x, int y) { return texelFetch(vram, ivec2(x & 1023, y & 511), 0).r; }
void main() {
    bool textured = (v_flags & 1u) != 0u, raw = (v_flags & 2u) != 0u, semi = (v_flags & 4u) != 0u;
    ivec3 c = ivec3(floor(v_col + 0.001));
    ivec3 c5;
    bool blended = semi;
    if (textured) {
        int u = int(floor(v_uv.x + 0.001)) & 255, v = int(floor(v_uv.y + 0.001)) & 255;
        int mx = int(v_twin.x), my = int(v_twin.y), ox = int(v_twin.z), oy = int(v_twin.w);
        u = (u & ~(mx * 8)) | ((ox & mx) * 8);
        v = (v & ~(my * 8)) | ((oy & my) * 8);
        int bx = int(v_tpage & 15u) * 64, by = int((v_tpage >> 4) & 1u) * 256 + v, mode = int((v_tpage >> 7) & 3u);
        int clx = int(v_clut & 63u) * 16, cly = int(v_clut >> 6);
        uint t;
        if (mode == 0) { uint w = tex16(bx + (u >> 2), by); t = tex16(clx + int((w >> uint((u & 3) * 4)) & 15u), cly); }
        else if (mode == 1) { uint w = tex16(bx + (u >> 1), by); t = tex16(clx + int((w >> uint((u & 1) * 8)) & 255u), cly); }
        else t = tex16(bx + u, by);
        if (t == 0u) discard;
        blended = semi && (t & 0x8000u) != 0u;
        ivec3 tc = ivec3(int(t & 31u), int((t >> 5) & 31u), int((t >> 10) & 31u));
        if (raw) c5 = tc;
        else c5 = min(((tc << 3) * c) >> 7, ivec3(255)) >> 3;
    } else {
        c5 = c >> 3;
    }
    if (u_pass == 1 && blended) discard;
    if (u_pass == 2 && !blended) discard;
    f_color = vec4(vec3((c5 << 3) | (c5 >> 2)) / 255.0, 1.0);
    // blended pixel: mode 0 = 0.5B + 0.5F, 1 = B + F, 3 = B + 0.25F;  opaque pixel (texel without STP, or a non-blended primitive): F
    if (!blended) f_blend = vec4(1.0, 1.0, 1.0, 0.0);
    else if (u_abr == 0) f_blend = vec4(0.5, 0.5, 0.5, 0.5);
    else if (u_abr == 3) f_blend = vec4(0.25, 0.25, 0.25, 1.0);
    else f_blend = vec4(1.0, 1.0, 1.0, 1.0);
}
"""

FULL_VS = """
#version 330
out vec2 v_c;
void main() {
    vec2 c = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));      // a triangle covering the screen: (0,0) (2,0) (0,2)
    v_c = c;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
"""

BLIT_FS = """
#version 330
uniform usampler2D vram;
uniform int u_s;
out vec4 f_color;
void main() {
    uint t = texelFetch(vram, ivec2(gl_FragCoord.xy) / u_s, 0).r;
    ivec3 c5 = ivec3(int(t & 31u), int((t >> 5) & 31u), int((t >> 10) & 31u));
    f_color = vec4(vec3((c5 << 3) | (c5 >> 2)) / 255.0, 1.0);
}
"""

SHOW_VS = """
#version 330
uniform vec4 u_rect;       // display area in VRAM pixels: x y w h
out vec2 v_uv;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    v_uv = vec2((u_rect.x + c.x * u_rect.z) / 1024.0, (u_rect.y + c.y * u_rect.w) / 512.0);
    gl_Position = vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
}
"""

SHOW_FS = """
#version 330
uniform sampler2D tex;
uniform float u_on;
in vec2 v_uv;
out vec4 f_color;
void main() { f_color = vec4(texture(tex, v_uv).rgb * u_on, 1.0); }
"""


def expand555(c5):
    return (c5 << 3) | (c5 >> 2)


class GLRenderer:
    def __init__(self, ctx, scale=1, smooth=False):
        self.ctx, self.S = ctx, int(scale)
        S = self.S
        self.vram = np.zeros((512, 1024), np.uint16)
        self.vram_tex = ctx.texture((1024, 512), 1, dtype='u2')
        self.vram_tex.filter = (moderngl.NEAREST, moderngl.NEAREST)
        self.hd_tex = ctx.texture((1024 * S, 512 * S), 4)
        self.hd_tex.filter = (moderngl.LINEAR if smooth else moderngl.NEAREST,) * 2
        self.fbo = ctx.framebuffer(self.hd_tex)
        self.fbo.clear(0.0, 0.0, 0.0, 1.0)
        self.vbo = ctx.buffer(reserve=24 * 110000, dynamic=True)
        self.prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
        self.prog['vram'] = 0
        self.prog['u_pass'] = 0
        self.prog['u_abr'] = 0
        self.vao = ctx.vertex_array(self.prog, [(self.vbo, '2i4 2i2 4u1 2u2 4u1', 'in_pos', 'in_uv', 'in_rgbf', 'in_ct', 'in_twin')])
        self.blit = ctx.program(vertex_shader=FULL_VS, fragment_shader=BLIT_FS)
        self.blit['vram'] = 0
        self.blit['u_s'] = S
        self.blit_vao = ctx.vertex_array(self.blit, [])
        self.show = ctx.program(vertex_shader=SHOW_VS, fragment_shader=SHOW_FS)
        self.show['tex'] = 1
        self.show_vao = ctx.vertex_array(self.show, [])
        self.disp = (0, 0, 256, 240, 0, 0)
        self.fb_rects = []                      # VRAM rectangles that are (or were) drawn into or displayed: uploads and copies touching them are mirrored in the HD texture
        self.stats = {'frames': 0, 'tris': 0, 'draws': 0, 'overflow': 0}

    # ---------------------------------------------------------------------------------------------------- helpers
    def _note_rect(self, r):
        if r not in self.fb_rects:
            self.fb_rects.append(r)
            if len(self.fb_rects) > 24:
                self.fb_rects.pop(0)

    def _touches_fb(self, x, y, w, h):
        for (rx, ry, rw, rh) in self.fb_rects:
            if x < rx + rw and rx < x + w and y < ry + rh and ry < y + h:
                return True
        return False

    def _vram_write(self, x, y, w, h, pix):
        """pix: (h, w) uint16 -> the CPU mirror and the 1x texture (clipped to the VRAM)."""
        x1, y1 = min(x + w, 1024), min(y + h, 512)
        if x1 <= x or y1 <= y:
            return
        sub = np.ascontiguousarray(pix[:y1 - y, :x1 - x])
        self.vram[y:y1, x:x1] = sub
        self.vram_tex.write(sub.tobytes(), viewport=(x, y, x1 - x, y1 - y))

    def _blit_hd(self, x, y, w, h):
        """The HD texture's rectangle <- the 1x VRAM texture (nearest)."""
        S = self.S
        x1, y1 = min(x + w, 1024), min(y + h, 512)
        if x1 <= x or y1 <= y:
            return
        self.fbo.use()
        self.ctx.viewport = (0, 0, 1024 * S, 512 * S)
        self.ctx.scissor = (x * S, y * S, (x1 - x) * S, (y1 - y) * S)
        self.ctx.disable(moderngl.BLEND)
        self.vram_tex.use(0)
        self.blit_vao.render(moderngl.TRIANGLES, vertices=3)
        self.ctx.scissor = None

    # ---------------------------------------------------------------------------------------------------- the trace
    def run(self, packet):
        """Replay one frame's trace (the bytes of gltrace_pack)."""
        ctx, S = self.ctx, self.S
        magic, nv, no, nd, over = struct.unpack_from('<5I', packet, 0)
        if magic != MAGIC:
            raise ValueError('not a GL trace')
        if over:
            self.stats['overflow'] += 1
        off = 20
        verts = packet[off:off + nv * 24]
        off += nv * 24
        ops = np.frombuffer(packet, dtype='<u4', count=no * 12, offset=off).reshape(no, 12)
        off += no * 48
        data = memoryview(packet)[off:off + nd]
        self._vnp = np.frombuffer(verts, VERT) if nv else None
        if nv:
            self.vbo.write(bytes(verts))
        for o in ops:
            t = int(o[0])
            if t == 1:
                self._draw(o)
            elif t == 2:
                x, y, w, h, d = (int(v) for v in o[1:6])
                pix = np.frombuffer(data, dtype='<u2', count=w * h, offset=d).reshape(h, w)
                self._vram_write(x, y, w, h, pix)
                if (w >= 1024 and h >= 512) or self._touches_fb(x, y, w, h):
                    self._blit_hd(x, y, w, h)
            elif t == 3:
                sx, sy, dx, dy, w, h = (int(v) for v in o[1:7])
                self._move(sx, sy, dx, dy, w, h)
            elif t == 4:
                x, y, w, h, rgb = (int(v) for v in o[1:6])
                c5 = ((rgb & 255) >> 3, ((rgb >> 8) & 255) >> 3, ((rgb >> 16) & 255) >> 3)
                v15 = c5[0] | (c5[1] << 5) | (c5[2] << 10)
                self._vram_write(x, y, w, h, np.full((h, w), v15, np.uint16))
                x1, y1 = min(x + w, 1024), min(y + h, 512)
                if x1 > x and y1 > y and self._touches_fb(x, y, w, h):
                    self.fbo.clear(expand555(c5[0]) / 255.0, expand555(c5[1]) / 255.0, expand555(c5[2]) / 255.0, 1.0, viewport=(x * S, y * S, (x1 - x) * S, (y1 - y) * S))
                self._note_rect((x, y, w, h)) if (w, h) != (1024, 512) and w * h > 64 * 64 else None
            elif t == 5:
                self.disp = tuple(int(v) for v in o[1:7])
                self._note_rect(self.disp[:4])
        self.stats['frames'] += 1

    def _move(self, sx, sy, dx, dy, w, h):
        S = self.S
        w, h = min(w, 1024 - max(sx, dx)), min(h, 512 - max(sy, dy))
        if w <= 0 or h <= 0:
            return
        src = self.vram[sy:sy + h, sx:sx + w].copy()
        self._vram_write(dx, dy, w, h, src)
        if self._touches_fb(sx, sy, w, h) or self._touches_fb(dx, dy, w, h):
            raw = self.fbo.read(viewport=(sx * S, sy * S, w * S, h * S), components=4)
            self.hd_tex.write(raw, viewport=(dx * S, dy * S, w * S, h * S))
            self._note_rect((dx, dy, w, h))

    def _draw(self, o):
        ctx, S = self.ctx, self.S
        first, count, blend = int(o[1]), int(o[2]), int(o[3])
        x1, y1, x2, y2 = (int(v) for v in o[4:8])
        if x2 < x1 or y2 < y1 or count == 0:
            return
        self._note_rect((x1, y1, x2 - x1 + 1, y2 - y1 + 1))
        self.fbo.use()
        ctx.viewport = (0, 0, 1024 * S, 512 * S)
        ctx.scissor = (x1 * S, y1 * S, (x2 - x1 + 1) * S, (y2 - y1 + 1) * S)
        self.vram_tex.use(0)
        self.stats['draws'] += 1
        self.stats['tris'] += count // 3
        if blend == 0:
            ctx.disable(moderngl.BLEND)
            self.prog['u_pass'] = 0
            self.vao.render(moderngl.TRIANGLES, vertices=count, first=first)
            return
        abr = blend - 1
        self.prog['u_abr'] = abr
        if abr != 2:
            # one pass in primitive order: the shader gives every pixel its own blend factors (source .rgb, destination .a: an opaque texel or a
            # non-blended pixel is F, a blended one 0.5B+0.5F / B+F / B+0.25F) -- dual-source blending
            ctx.enable(moderngl.BLEND)
            ctx.blend_equation = moderngl.FUNC_ADD
            ctx.blend_func = SRC1_COLOR, SRC1_ALPHA
            self.prog['u_pass'] = 0
            self.vao.render(moderngl.TRIANGLES, vertices=count, first=first)
            ctx.disable(moderngl.BLEND)
            return
        # B-F: an opaque pixel (+F) and a blended one (B-F) need different equations, so the draw is cut into runs of textured / untextured primitives
        # (primitive order is kept between runs; inside a textured run opaque texels go first)
        tex = self._vnp['rgbf'][first:first + count:3, 3] & 1
        cuts = [0] + [int(c) for c in np.flatnonzero(np.diff(tex)) + 1] + [count // 3]
        for lo, hi in zip(cuts[:-1], cuts[1:]):
            f, n = first + 3 * lo, 3 * (hi - lo)
            if tex[lo]:
                ctx.disable(moderngl.BLEND)
                self.prog['u_pass'] = 1
                self.vao.render(moderngl.TRIANGLES, vertices=n, first=f)
            ctx.enable(moderngl.BLEND)
            ctx.blend_equation = moderngl.FUNC_REVERSE_SUBTRACT
            ctx.blend_func = moderngl.ONE, moderngl.ONE
            self.prog['u_pass'] = 2
            self.vao.render(moderngl.TRIANGLES, vertices=n, first=f)
        ctx.blend_equation = moderngl.FUNC_ADD
        ctx.disable(moderngl.BLEND)

    # ---------------------------------------------------------------------------------------------------- output
    def present(self, target, width, height):
        """Show the display area on `target` (the window's framebuffer), letter-boxed to 4:3."""
        ctx = self.ctx
        x, y, w, h, on, _ = self.disp
        w, h = max(w, 1), max(h, 1)
        target.use()
        ctx.scissor = None
        ctx.disable(moderngl.BLEND)
        target.clear(0.0, 0.0, 0.0, 1.0)
        aspect = 4.0 / 3.0
        vw, vh = (width, int(width / aspect)) if width / height < aspect else (int(height * aspect), height)
        ctx.viewport = ((width - vw) // 2, (height - vh) // 2, vw, vh)
        self.show['u_rect'] = (float(x), float(y), float(w), float(h))
        self.show['u_on'] = 1.0 if on else 0.0
        self.hd_tex.use(1)
        self.show_vao.render(moderngl.TRIANGLE_STRIP, vertices=4)

    def read_display_rgb(self):
        """The display area at the renderer's resolution as an (h*S, w*S, 3) uint8 array, top row first."""
        x, y, w, h, on, _ = self.disp
        S = self.S
        raw = self.fbo.read(viewport=(x * S, y * S, w * S, h * S), components=3)
        a = np.frombuffer(raw, np.uint8).reshape(h * S, w * S, 3)
        return a if on else np.zeros_like(a)
