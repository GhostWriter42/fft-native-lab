#ifndef NATIVE_GTE_H
#define NATIVE_GTE_H
/* Software GTE (PS1 geometry coprocessor) for the native FFT port.
 *
 * Deterministic integer implementation of the register file and the commands the game and its libgte actually use
 * (MVMVA, RTPS, RTPT, NCLIP, SQR, AVSZ3, AVSZ4, OP) plus LZCS/LZCR. Written from the public hardware documentation
 * (psx-spx style behaviour), not copied from any emulator.
 *
 * The perspective divide (H / SZ3) uses the hardware's reciprocal-table (UNR) algorithm as documented in psx-spx (checked
 * against exact division in the unit tests, not yet against a real console or an emulator trace); define GTE_EXACT_DIVIDE
 * to use true integer division instead (differs from the hardware by at most 1 in rare cases).
 *
 * Freestanding: no libc. Types are local so the file builds both -m32 (game) and 64-bit (tests). */

typedef signed char gte_s8;
typedef signed short gte_s16;
typedef unsigned short gte_u16;
typedef signed int gte_s32;
typedef unsigned int gte_u32;
typedef long long gte_s64;
typedef unsigned long long gte_u64;

/* control register numbers (cop2 control side) */
enum {
    GTE_C_R11R12 = 0, GTE_C_R13R21, GTE_C_R22R23, GTE_C_R31R32, GTE_C_R33,
    GTE_C_TRX, GTE_C_TRY, GTE_C_TRZ,
    GTE_C_L11L12, GTE_C_L13L21, GTE_C_L22L23, GTE_C_L31L32, GTE_C_L33,
    GTE_C_RBK, GTE_C_GBK, GTE_C_BBK,
    GTE_C_LR1LR2, GTE_C_LR3LG1, GTE_C_LG2LG3, GTE_C_LB1LB2, GTE_C_LB3,
    GTE_C_RFC, GTE_C_GFC, GTE_C_BFC,
    GTE_C_OFX, GTE_C_OFY, GTE_C_H, GTE_C_DQA, GTE_C_DQB, GTE_C_ZSF3, GTE_C_ZSF4, GTE_C_FLAG
};
/* data register numbers (cop2 data side) */
enum {
    GTE_D_VXY0 = 0, GTE_D_VZ0, GTE_D_VXY1, GTE_D_VZ1, GTE_D_VXY2, GTE_D_VZ2, GTE_D_RGBC, GTE_D_OTZ,
    GTE_D_IR0, GTE_D_IR1, GTE_D_IR2, GTE_D_IR3, GTE_D_SXY0, GTE_D_SXY1, GTE_D_SXY2, GTE_D_SXYP,
    GTE_D_SZ0, GTE_D_SZ1, GTE_D_SZ2, GTE_D_SZ3, GTE_D_RGB0, GTE_D_RGB1, GTE_D_RGB2, GTE_D_RES1,
    GTE_D_MAC0, GTE_D_MAC1, GTE_D_MAC2, GTE_D_MAC3, GTE_D_IRGB, GTE_D_ORGB, GTE_D_LZCS, GTE_D_LZCR
};

/* FLAG register bits */
enum {
    GTE_F_ERROR = 1u << 31,
    GTE_F_MAC1_OF = 1u << 30, GTE_F_MAC2_OF = 1u << 29, GTE_F_MAC3_OF = 1u << 28,
    GTE_F_MAC1_UF = 1u << 27, GTE_F_MAC2_UF = 1u << 26, GTE_F_MAC3_UF = 1u << 25,
    GTE_F_IR1_SAT = 1u << 24, GTE_F_IR2_SAT = 1u << 23, GTE_F_IR3_SAT = 1u << 22,
    GTE_F_RGB_R_SAT = 1u << 21, GTE_F_RGB_G_SAT = 1u << 20, GTE_F_RGB_B_SAT = 1u << 19,
    GTE_F_SZ3_OTZ_SAT = 1u << 18, GTE_F_DIV_OF = 1u << 17,
    GTE_F_MAC0_OF = 1u << 16, GTE_F_MAC0_UF = 1u << 15,
    GTE_F_SX2_SAT = 1u << 14, GTE_F_SY2_SAT = 1u << 13, GTE_F_IR0_SAT = 1u << 12
};

typedef struct {
    gte_s32 ctrl[32];   /* raw control registers (packed exactly as the hardware exposes them) */
    gte_s32 data[32];   /* raw data registers (see gte_mfc2/gte_mtc2 for the special ones) */
    gte_u32 flag;
} gte_state_t;

extern gte_state_t g_gte;

void gte_reset(void);
void gte_ctc2(int reg, gte_u32 value);        /* write a control register  */
gte_u32 gte_cfc2(int reg);                    /* read a control register   */
void gte_mtc2(int reg, gte_u32 value);        /* write a data register     */
gte_u32 gte_mfc2(int reg);                    /* read a data register      */
void gte_command(gte_u32 cmd);                /* execute a GTE command word (the 25-bit cop2 field or the full 0x4a.. word) */
gte_u32 gte_divide_unr(gte_u32 h, gte_u32 sz3);    /* the two perspective-divide quotients (see gte.c); h < 2*sz3, sz3 != 0 */
gte_u32 gte_divide_exact(gte_u32 h, gte_u32 sz3);

/* convenience for callers that move whole vectors */
void gte_load_sv(int v, const void* svector);      /* V0/V1/V2 <- SVECTOR (vx,vy,vz,pad as 2 words) */
void gte_load_ir(gte_s32 x, gte_s32 y, gte_s32 z); /* IR1-3 */

/* command word decode helpers */
#define GTE_CMD_OP(c)  ((c) & 0x3f)
#define GTE_CMD_LM(c)  (((c) >> 10) & 1)
#define GTE_CMD_TX(c)  (((c) >> 13) & 3)
#define GTE_CMD_VX(c)  (((c) >> 15) & 3)
#define GTE_CMD_MX(c)  (((c) >> 17) & 3)
#define GTE_CMD_SF(c)  (((c) >> 19) & 1)

enum { GTE_OP_RTPS = 0x01, GTE_OP_NCLIP = 0x06, GTE_OP_OP = 0x0c, GTE_OP_MVMVA = 0x12, GTE_OP_SQR = 0x28,
       GTE_OP_AVSZ3 = 0x2d, GTE_OP_AVSZ4 = 0x2e, GTE_OP_RTPT = 0x30, GTE_OP_GPF = 0x3d };

#endif
