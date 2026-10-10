/* A software model of the PlayStation SPU -- see spu.h. The register semantics follow the decomp's own libspu sources (src/psyq/libspu): the SDK calls below do to the model's
 * registers exactly what those functions do to the hardware's. */
#include "hle.h"
#include "spu.h"

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned rd32(hle_t* h, unsigned addr) {
    return h->p.r8(h->p.ctx, addr) | (h->p.r8(h->p.ctx, addr + 1) << 8) | (h->p.r8(h->p.ctx, addr + 2) << 16) | (h->p.r8(h->p.ctx, addr + 3) << 24);
}
static int rd16s(hle_t* h, unsigned addr) { return (short)(h->p.r8(h->p.ctx, addr) | (h->p.r8(h->p.ctx, addr + 1) << 8)); }

void spu_reset(spu_t* s) {
    unsigned i;
    for (i = 0; i < sizeof *s; i++) ((unsigned char*)s)[i] = 0;
    s->master_l = s->master_r = 0x3fff;
}

static unsigned vol_reg(int v, int mode) {                                      /* SpuSetVoiceVolume(Attr): a sweep mode keeps the target (0..127) in the low bits; played as a fixed level */
    if (mode) { v &= 0x7f; return (unsigned)(v << 7) & 0x7fffu; }
    return (unsigned)v & 0x7fffu;
}

int spu_hle_call(hle_t* h, spu_t* s, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret) {
    spu_voice_t* v = a0 < SPU_VOICES ? &s->v[a0] : 0;
    *ret = 0;
    if (n[0] != 'S' || n[1] != 'p' || n[2] != 'u') return 0;
    if (streq(n, "SpuSetVoiceVolume")) { if (v) { v->vol_l = (unsigned short)vol_reg((short)a1, 0); v->vol_r = (unsigned short)vol_reg((short)a2, 0); } return 1; }
    if (streq(n, "SpuSetVoiceVolumeAttr")) { if (v) { v->vol_l = (unsigned short)vol_reg((short)a1, (short)a3 != 0); v->vol_r = (unsigned short)vol_reg((short)a2, 0); } return 1; }   /* the right mode is a stack argument: fixed */
    if (streq(n, "SpuSetVoicePitch")) { if (v) v->pitch = (unsigned short)a1; return 1; }
    if (streq(n, "SpuSetVoiceStartAddr")) { if (v) v->start = (unsigned short)(a1 >> 3); return 1; }
    if (streq(n, "SpuSetVoiceLoopStartAddr")) { if (v) v->loop = (unsigned short)(a1 >> 3); return 1; }
    if (streq(n, "SpuSetVoiceARAttr")) { if (v) v->adsr1 = (unsigned short)((v->adsr1 & 0xff) | (((a1 & 0xffffu) | (a2 == 5 ? 0x80u : 0u)) << 8)); return 1; }
    if (streq(n, "SpuSetVoiceDR")) { if (v) v->adsr1 = (unsigned short)((v->adsr1 & 0xff0f) | ((a1 & 0xffffu) << 4)); return 1; }
    if (streq(n, "SpuSetVoiceSL")) { if (v) v->adsr1 = (unsigned short)((v->adsr1 & 0xfff0) | (a1 & 0xffffu)); return 1; }
    if (streq(n, "SpuSetVoiceSRAttr")) {
        unsigned mode = 0x100;
        if (a2 == 1) mode = 0; else if (a2 == 5) mode = 0x200; else if (a2 == 7) mode = 0x300;
        if (v) v->adsr2 = (unsigned short)((v->adsr2 & 0x3f) | (((a1 & 0xffffu) | mode) << 6));
        return 1;
    }
    if (streq(n, "SpuSetVoiceRRAttr")) { if (v) v->adsr2 = (unsigned short)((v->adsr2 & 0xffc0) | (a1 & 0xffffu) | (a2 != 3 && a2 == 7 ? 0x20u : 0u)); return 1; }
    if (streq(n, "SpuSetVoiceRR")) { if (v) v->adsr2 = (unsigned short)((v->adsr2 & 0xffc0) | (a1 & 0xffffu)); return 1; }
    if (streq(n, "SpuSetKey")) {                                                /* (on_off, voice bits) */
        unsigned bits = a1 & 0xffffffu, i;
        for (i = 0; i < SPU_VOICES; i++) {
            spu_voice_t* w = &s->v[i];
            if (!(bits & (1u << i))) continue;
            if (a0 == 1) {                                                       /* key on: restart at the start address */
                w->on = 1; w->phase = 0; w->level = 0; w->counter = 0;
                w->addr = (unsigned)w->start << 3; w->loop_addr = w->addr; w->pos = 0; w->hist1 = w->hist2 = 0; w->blocks = -1;
                s->keystat |= 1u << i; s->n_keyons++;
            } else if (a0 == 0) { if (w->on) w->phase = 3; s->keystat &= ~(1u << i); }
        }
        return 1;
    }
    if (streq(n, "SpuSetTransferStartAddr")) { if (a0 - 0x1010u <= 0x7efe8u) s->tsa = a0; return 1; }            /* (returns 0 like the generic path: the model must not change what the game sees) */
    if (streq(n, "SpuWrite") || streq(n, "SpuWrite0")) {                          /* (buffer, size): main RAM -> sound RAM at the transfer address; the generic code runs the transfer callback */
        unsigned size = a1 > 0x7eff0u ? 0x7eff0u : a1, i;
        for (i = 0; i < size && s->tsa + i < SPU_RAM_BYTES; i++) s->ram[s->tsa + i] = (unsigned char)h->p.r8(h->p.ctx, a0 + i);
        s->tsa += size; s->n_writes++;
        return 0;
    }
    if (streq(n, "SpuSetReverbModeParam")) {                                      /* (attr): the mode's preset registers come from the game's own table */
        unsigned mask = rd32(h, a0);
        if ((mask == 0 || (mask & 1)) && s->rev_param) {
            unsigned mode = rd32(h, a0 + 4) & 0xffu, i;
            if (mode < 10) {
                unsigned p = s->rev_param + mode * 68u + 4u;
                for (i = 0; i < 32; i++) s->rev_reg[i] = (unsigned short)(h->p.r8(h->p.ctx, p + 2 * i) | (h->p.r8(h->p.ctx, p + 2 * i + 1) << 8));
                s->rev_base = rd32(h, s->rev_start + mode * 4u) << 3;
                s->rev_cur = s->rev_base;
            }
        }
        return 0;
    }
    if (streq(n, "SpuSetReverbDepth")) {
        unsigned mask = rd32(h, a0);
        if (mask == 0 || (mask & 2)) s->rev_vol_l = rd16s(h, a0 + 8);
        if (mask == 0 || (mask & 4)) s->rev_vol_r = rd16s(h, a0 + 10);
        return 0;
    }
    if (streq(n, "SpuSetReverb")) { if (a0 == 1) s->rev_on = 1; else if (a0 == 0) s->rev_on = 0; return 0; }
    if (streq(n, "SpuSetReverbVoice")) { if (a0 == 1) s->rev_voices |= a1 & 0xffffffu; else if (a0 == 0) s->rev_voices &= ~(a1 & 0xffffffu); return 0; }
    if (streq(n, "SpuClearReverbWorkArea")) {
        if (a0 < 10 && s->rev_start) { unsigned b = rd32(h, s->rev_start + a0 * 4u) << 3, i; for (i = b; i < SPU_RAM_BYTES; i++) s->ram[i] = 0; }
        return 0;
    }
    if (streq(n, "SpuSetNoiseVoice") || streq(n, "SpuSetPitchLFOVoice")) {       /* (on_off, voice bits): 1 = add, 0 = remove, 8 = exactly these (_SpuSetAnyVoice) */
        unsigned* m = n[6] == 'N' ? &s->noise_voices : &s->pmod_voices, bits = a1 & 0xffffffu;
        if (a0 == 1) *m |= bits; else if (a0 == 0) *m &= ~bits; else if (a0 == 8) *m = bits;
        return 1;                                                               /* (the result stays the generic 0: the model must not change what the game sees) */
    }
    if (streq(n, "SpuSetNoiseClock")) { int c = (int)a0; s->noise_clock = c < 0 ? 0 : c > 63 ? 63 : c; return 1; }
    if (streq(n, "SpuSetCommonAttr")) {                                         /* the main volume (mask bit 0 / all) */
        unsigned mask = rd32(h, a0);
        if (mask == 0 || (mask & 1)) {
            s->master_l = (int)vol_reg(rd16s(h, a0 + 4), rd16s(h, a0 + 8) != 0 && (mask == 0 || (mask & 4)));
            s->master_r = (int)vol_reg(rd16s(h, a0 + 6), rd16s(h, a0 + 10) != 0 && (mask == 0 || (mask & 4)));
        }
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------ synthesis */
static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
static int decode_into(spu_t* s, spu_voice_t* v, unsigned addr, short* out, int* flags_out) {      /* the 16-byte ADPCM block at `addr` -> 28 samples; the ADPCM history runs on from block to block */
    const unsigned char* b;
    int shift, filter, i;
    unsigned flags;
    if (addr + 16 > SPU_RAM_BYTES) return 0;
    b = s->ram + addr;
    shift = b[0] & 15; filter = (b[0] >> 4) & 7; flags = b[1];
    if (shift > 12) shift = 9;                                                  /* the hardware treats 13..15 like 9 */
    if (filter > 4) filter = 4;
    if (flags & 4) v->loop_addr = addr;
    for (i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15, smp;
        smp = (short)(nib << 12) >> shift;
        smp += (v->hist1 * f0[filter] + v->hist2 * f1[filter] + 32) >> 6;
        if (smp > 32767) smp = 32767; else if (smp < -32768) smp = -32768;
        v->hist2 = v->hist1; v->hist1 = smp;
        out[i] = (short)smp;
    }
    *flags_out = (int)flags;
    return 1;
}
static void prefetch_next(spu_t* s, spu_voice_t* v) {                           /* decode the block that follows the current one (what the interpolation needs past the end) */
    unsigned naddr;
    v->nvalid = 0;
    if (v->blocks & 1) {
        if (!(v->blocks & 2)) return;                                           /* end without repeat: nothing follows */
        naddr = v->loop_addr;
    } else naddr = v->addr + 16;
    if (decode_into(s, v, naddr, v->nbuf, &v->nflags)) { v->naddr = naddr; v->nvalid = 1; }
}
static void decode_block(spu_t* s, spu_voice_t* v) {                           /* a freshly keyed voice: its first block and the one after it */
    int flags, i;
    for (i = 0; i < 3; i++) v->buf[i] = 0;
    if (!decode_into(s, v, v->addr, v->buf + 3, &flags)) { v->on = 0; return; }
    v->blocks = flags;
    prefetch_next(s, v);
}
static void next_block(spu_t* s, spu_voice_t* v) {
    int i;
    if (!v->nvalid) { v->on = 0; v->level = 0; return; }                        /* end without repeat: silence */
    for (i = 0; i < 3; i++) v->buf[i] = v->buf[28 + i];
    for (i = 0; i < 28; i++) v->buf[3 + i] = v->nbuf[i];
    v->addr = v->naddr; v->blocks = v->nflags;
    prefetch_next(s, v);
}
static int rate(int shift, int* step) {                                         /* ADSR: cycles per step for a shift, step scaled in place */
    int c = shift > 11 ? 1 << (shift - 11) : 1;
    if (shift < 11) *step <<= 11 - shift;
    return c;
}
static void env_tick(spu_voice_t* v) {
    int step, cycles, exp, shift;
    switch (v->phase) {
    case 0:
        exp = (v->adsr1 >> 15) & 1; shift = (v->adsr1 >> 10) & 31; step = 7 - ((v->adsr1 >> 8) & 3);
        cycles = rate(shift, &step);
        if (exp && v->level > 0x6000) cycles *= 4;
        if (++v->counter >= cycles) { v->counter = 0; v->level += step; if (v->level >= 0x7fff) { v->level = 0x7fff; v->phase = 1; } }
        break;
    case 1:
        shift = (v->adsr1 >> 4) & 15; step = -8;
        cycles = rate(shift, &step);
        if (++v->counter >= cycles) {
            int sl = ((v->adsr1 & 15) + 1) * 0x800;
            v->counter = 0; step = (step * v->level) >> 15; v->level += step;
            if (v->level < 0) v->level = 0;
            if (v->level <= sl) v->phase = 2;
        }
        break;
    case 2: {
        int dec = (v->adsr2 >> 14) & 1;
        exp = (v->adsr2 >> 15) & 1; shift = (v->adsr2 >> 8) & 31; step = (v->adsr2 >> 6) & 3;
        step = dec ? -8 + step : 7 - step;
        cycles = rate(shift, &step);
        if (exp && !dec && v->level > 0x6000) cycles *= 4;
        if (++v->counter >= cycles) {
            v->counter = 0;
            if (exp && dec) step = (step * v->level) >> 15;
            v->level += step;
            if (v->level < 0) v->level = 0; else if (v->level > 0x7fff) v->level = 0x7fff;
        }
        break; }
    default:
        exp = (v->adsr2 >> 5) & 1; shift = v->adsr2 & 31; step = -8;
        cycles = rate(shift, &step);
        if (++v->counter >= cycles) {
            v->counter = 0;
            if (exp) step = (step * v->level) >> 15;
            v->level += step;
            if (v->level <= 0) { v->level = 0; v->on = 0; }
        }
        break;
    }
}
static int gain(unsigned reg) { return (reg & 0x4000) ? (int)reg - 0x8000 : (int)reg; }      /* 15-bit signed fixed volume: +-0x4000 = unity */

/* The reverb unit (nocash / Mednafen description): every other sample (22.05 kHz) the input of the reverb voices goes through the same-side / different-side reflection filters,
 * four comb taps and two all-pass stages, working in the sound RAM from rev_base on; all addresses are 8-byte units added to a pointer that advances two bytes per cycle. */
static int sat(int x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
static int rv_rd(spu_t* s, int units) {                                         /* the 16-bit word at (cur + units * 8), wrapped into the work area */
    int size = (int)(SPU_RAM_BYTES - s->rev_base), a;
    if (size <= 0) return 0;
    a = (int)(s->rev_cur - s->rev_base) + units * 8;
    a %= size; if (a < 0) a += size;
    a += (int)s->rev_base;
    return (short)(s->ram[a] | (s->ram[a + 1] << 8));
}
static void rv_wr(spu_t* s, int units, int v) {
    int size = (int)(SPU_RAM_BYTES - s->rev_base), a;
    if (size <= 0) return;
    a = (int)(s->rev_cur - s->rev_base) + units * 8;
    a %= size; if (a < 0) a += size;
    a += (int)s->rev_base;
    s->ram[a] = (unsigned char)v; s->ram[a + 1] = (unsigned char)(v >> 8);
}
enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
       mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };
#define RG(i) ((int)(unsigned short)s->rev_reg[i])
#define VG(i) ((int)(short)s->rev_reg[i])
static int rv_prev(spu_t* s, int reg) {                                         /* [reg - 2 bytes] */
    int size = (int)(SPU_RAM_BYTES - s->rev_base), a;
    if (size <= 0) return 0;
    a = (int)(s->rev_cur - s->rev_base) + RG(reg) * 8 - 2;
    a %= size; if (a < 0) a += size;
    a += (int)s->rev_base;
    return (short)(s->ram[a] | (s->ram[a + 1] << 8));
}
static void reverb_cycle(spu_t* s, int in_l, int in_r) {
    int lin = (VG(vLIN) * in_l) >> 15, rin = (VG(vRIN) * in_r) >> 15, wall = VG(vWALL), iir = VG(vIIR), t, lout, rout;
    if (s->rev_base == 0) return;
    t = lin + ((rv_rd(s, RG(dLSAME)) * wall) >> 15) - rv_prev(s, mLSAME); t = ((t * iir) >> 15) + rv_prev(s, mLSAME); rv_wr(s, RG(mLSAME), sat(t));
    t = rin + ((rv_rd(s, RG(dRSAME)) * wall) >> 15) - rv_prev(s, mRSAME); t = ((t * iir) >> 15) + rv_prev(s, mRSAME); rv_wr(s, RG(mRSAME), sat(t));
    t = lin + ((rv_rd(s, RG(dRDIFF)) * wall) >> 15) - rv_prev(s, mLDIFF); t = ((t * iir) >> 15) + rv_prev(s, mLDIFF); rv_wr(s, RG(mLDIFF), sat(t));
    t = rin + ((rv_rd(s, RG(dLDIFF)) * wall) >> 15) - rv_prev(s, mRDIFF); t = ((t * iir) >> 15) + rv_prev(s, mRDIFF); rv_wr(s, RG(mRDIFF), sat(t));
    lout = ((VG(vCOMB1) * rv_rd(s, RG(mLCOMB1))) >> 15) + ((VG(vCOMB2) * rv_rd(s, RG(mLCOMB2))) >> 15) + ((VG(vCOMB3) * rv_rd(s, RG(mLCOMB3))) >> 15) + ((VG(vCOMB4) * rv_rd(s, RG(mLCOMB4))) >> 15);
    rout = ((VG(vCOMB1) * rv_rd(s, RG(mRCOMB1))) >> 15) + ((VG(vCOMB2) * rv_rd(s, RG(mRCOMB2))) >> 15) + ((VG(vCOMB3) * rv_rd(s, RG(mRCOMB3))) >> 15) + ((VG(vCOMB4) * rv_rd(s, RG(mRCOMB4))) >> 15);
    lout = sat(lout); rout = sat(rout);
    lout = sat(lout - ((VG(vAPF1) * rv_rd(s, RG(mLAPF1) - RG(dAPF1))) >> 15)); rv_wr(s, RG(mLAPF1), lout); lout = sat(((lout * VG(vAPF1)) >> 15) + rv_rd(s, RG(mLAPF1) - RG(dAPF1)));
    rout = sat(rout - ((VG(vAPF1) * rv_rd(s, RG(mRAPF1) - RG(dAPF1))) >> 15)); rv_wr(s, RG(mRAPF1), rout); rout = sat(((rout * VG(vAPF1)) >> 15) + rv_rd(s, RG(mRAPF1) - RG(dAPF1)));
    lout = sat(lout - ((VG(vAPF2) * rv_rd(s, RG(mLAPF2) - RG(dAPF2))) >> 15)); rv_wr(s, RG(mLAPF2), lout); lout = sat(((lout * VG(vAPF2)) >> 15) + rv_rd(s, RG(mLAPF2) - RG(dAPF2)));
    rout = sat(rout - ((VG(vAPF2) * rv_rd(s, RG(mRAPF2) - RG(dAPF2))) >> 15)); rv_wr(s, RG(mRAPF2), rout); rout = sat(((rout * VG(vAPF2)) >> 15) + rv_rd(s, RG(mRAPF2) - RG(dAPF2)));
    s->rev_out_l = lout; s->rev_out_r = rout;
    s->rev_cur += 2; if (s->rev_cur >= SPU_RAM_BYTES) s->rev_cur = s->rev_base;
}

/* The noise generator (psx-spx, "SPU Noise Generator"): a 16-bit shift register fed with the parity of bits 15, 12, 11, 10 (inverted), stepped by a timer whose rate the
 * clock (shift = clock >> 2, step = (clock & 3) + 4) sets; one call per 44.1 kHz sample. */
static void noise_tick(spu_t* s) {
    int shift = s->noise_clock >> 2, step = (s->noise_clock & 3) + 4, lv = s->noise_level;
    int parity = ((lv >> 15) ^ (lv >> 12) ^ (lv >> 11) ^ (lv >> 10) ^ 1) & 1;
    s->noise_timer -= step;
    if (s->noise_timer < 0) {
        s->noise_level = (short)((lv << 1) + parity);
        s->noise_timer += 0x20000 >> shift;
        if (s->noise_timer < 0) s->noise_timer += 0x20000 >> shift;
    }
}

void spu_mix(spu_t* s, short* out, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        int l = 0, r = 0, rl = 0, rr = 0;
        if (s->noise_voices) { noise_tick(s); s->n_noise_frames++; }
        if (s->pmod_voices) s->n_pmod_frames++;
        for (k = 0; k < SPU_VOICES; k++) {
            spu_voice_t* v = &s->v[k];
            int idx, frac, smp, vl, vr;
            if (!v->on) { v->last_out = 0; continue; }
            if (v->blocks == -1) decode_block(s, v);
            if (!v->on) { v->last_out = 0; continue; }
            idx = (int)(v->pos >> 12); frac = (int)(v->pos & 0xfff);
            {   /* 4-point cubic (Catmull-Rom) through the samples idx-1 .. idx+2, which may lie in the previous / next block: no steps at block boundaries */
                int p0 = v->buf[idx + 2], p1 = v->buf[idx + 3], p2, p3;
                float x = (float)frac * (1.0f / 4096.0f), r;
                p2 = idx + 1 < 28 ? v->buf[idx + 4] : (v->nvalid ? v->nbuf[idx + 1 - 28] : p1);
                p3 = idx + 2 < 28 ? v->buf[idx + 5] : (v->nvalid ? v->nbuf[idx + 2 - 28] : p2);
                r = (float)p1 + 0.5f * x * ((float)(p2 - p0) + x * ((float)(2 * p0 - 5 * p1 + 4 * p2 - p3) + x * (float)(3 * (p1 - p2) + p3 - p0)));
                smp = r > 32767.0f ? 32767 : (r < -32768.0f ? -32768 : (int)r);
            }
            if (s->noise_voices & (1u << k)) smp = (short)s->noise_level;        /* a noise voice plays the generator; its ADPCM still runs (loop / end flags) */
            env_tick(v);
            smp = (smp * v->level) >> 15;
            v->last_out = smp;
            vl = (smp * gain(v->vol_l)) >> 14; vr = (smp * gain(v->vol_r)) >> 14;
            l += vl; r += vr;
            if (s->rev_voices & (1u << k)) { rl += vl; rr += vr; }
            {   unsigned step = v->pitch;
                if (k > 0 && (s->pmod_voices & (1u << k))) {                      /* pitch modulation: step * (previous voice's output + 0x8000) / 0x8000 */
                    step = ((unsigned)((int)step * (s->v[k - 1].last_out + 0x8000)) >> 15) & 0xffffu;
                    if (step > 0x3fff) step = 0x4000;
                }
                v->pos += step > 0x3fff ? 0x3fff : step;
            }
            if (v->pos >= (28u << 12)) { v->pos -= 28u << 12; next_block(s, v); }
        }
        if (s->rev_on) {
            if ((s->rev_phase ^= 1) == 0) reverb_cycle(s, sat(rl), sat(rr));
            l += (s->rev_out_l * s->rev_vol_l) >> 15; r += (s->rev_out_r * s->rev_vol_r) >> 15;
        }
        l = (l * s->master_l) >> 14; r = (r * s->master_r) >> 14;
        out[2 * i] = (short)sat(l);
        out[2 * i + 1] = (short)sat(r);
    }
}
