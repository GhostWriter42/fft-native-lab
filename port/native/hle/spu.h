#ifndef SPU_H
#define SPU_H
/* A software model of the PlayStation SPU, driven at the level of the libspu SDK calls the game makes (the same level as the GPU model): 24 ADPCM voices with pitch, the
 * ADSR envelope, per-voice and master volume, key on / off, the transfer of sample data into the 512 KiB sound RAM, mixed to 44.1 kHz stereo. Not modelled: the reverb unit,
 * noise, pitch modulation, volume sweeps, CD audio. The model never feeds anything back to the game (voice status is derived), so it cannot change the game's behaviour:
 * it only turns the SDK calls into sound. Freestanding C. */

#define SPU_RAM_BYTES (512 * 1024)
#define SPU_VOICES 24

typedef struct spu_voice {
    unsigned short vol_l, vol_r, pitch, start, adsr1, adsr2, loop;              /* the registers (start / loop in units of 8 bytes, volumes as written: 15 bits, bit 14 = sign) */
    int on;                                                                     /* playing (keyed on and not yet silent) */
    int phase;                                                                  /* 0 attack, 1 decay, 2 sustain, 3 release */
    int level;                                                                  /* envelope 0..0x7fff */
    int counter;                                                                /* envelope rate counter */
    unsigned addr;                                                              /* byte address of the current ADPCM block */
    unsigned loop_addr;                                                         /* byte address the next loop jumps to */
    unsigned pos;                                                               /* 12-bit fractional sample position inside the 28 decoded samples */
    int hist1, hist2;                                                           /* ADPCM history */
    short buf[28 + 3];                                                          /* buf[0..2] = the last 3 samples of the previous block, buf[3..30] = the 28 decoded samples of the current one */
    int blocks;                                                                 /* flags of the current block (bit 0 end, bit 1 repeat, bit 2 loop start); -1 = nothing decoded yet */
    short nbuf[28];                                                             /* the NEXT block, decoded ahead so that the interpolation can look past the end of the current one */
    int nflags, nvalid;
    unsigned naddr;
} spu_voice_t;

typedef struct spu {
    unsigned char ram[SPU_RAM_BYTES];
    spu_voice_t v[SPU_VOICES];
    unsigned tsa;                                                               /* transfer start address (bytes) */
    int master_l, master_r;                                                     /* main volume, 15-bit */
    unsigned rev_param, rev_start;                                              /* the game's _spu_rev_param[10] (68 bytes each: mask + 32 register values) and _spu_rev_startaddr[10] tables (set by the driver) */
    int rev_on, rev_vol_l, rev_vol_r;                                           /* reverb enabled, output volumes (SpuSetReverbDepth) */
    unsigned rev_voices;                                                        /* voices that feed the reverb */
    unsigned short rev_reg[32];                                                 /* the reverb registers of the current mode (dAPF1 ... vRIN) */
    unsigned rev_base, rev_cur;                                                 /* work area start (bytes) and the moving buffer pointer */
    int rev_out_l, rev_out_r, rev_phase;
    unsigned keystat;                                                           /* voices keyed on */
    unsigned n_writes, n_keyons;                                                /* statistics */
} spu_t;

struct hle;
void spu_reset(spu_t* s);
/* The libspu calls (SpuSetVoiceVolume, SpuSetKey, SpuWrite ...): returns 1 when `name` was handled (result in *ret). */
int spu_hle_call(struct hle* h, spu_t* s, const char* name, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret);
/* Produce n stereo frames (44.1 kHz, interleaved left / right, signed 16-bit). */
void spu_mix(spu_t* s, short* out, int n);
#endif
