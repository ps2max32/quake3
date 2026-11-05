/* the layout of q3stagec.vsm, by hand: mxglc.h's, with the clipping's uniforms named */
#ifndef Q3STAGEC_H
#define Q3STAGEC_H
/* vertex program mxglv: VU data memory layout, in quadwords */
#define VU_Q3STAGEC_MVP 0
#define VU_Q3STAGEC_CSCALE 4
#define VU_Q3STAGEC_TMS 5
#define VU_Q3STAGEC_TMT 6
#define VU_Q3STAGEC_CMUL 7
#define VU_Q3STAGEC_CADD 8
#define VU_Q3STAGEC_CLIP 9
#define VU_Q3STAGEC_ENTRY 0  /* micro address, 64-bit units; MSCAL (ITOP = the context) for the first batch of a draw, MSCNT for the others */
#define VU_Q3STAGEC_BUF_QW 432  /* double buffer size */
#define VU_Q3STAGEC_UNI_QW 17  /* uniforms, after the context's GIF tag */
#define VU_Q3STAGEC_CTX_AT 864  /* the contexts: GIF tag (NLOOP 0), uniforms */
#define VU_Q3STAGEC_CTX_QW 18  /* per context */
#define VU_Q3STAGEC_NCTX 2  /* contexts (1: a FLUSHE before writing it) */
#define VU_Q3STAGEC_ITEM_QW 3  /* quadwords per vertex in */
#define VU_Q3STAGEC_OUT_QW 3  /* GIF registers per vertex */
#define VU_Q3STAGEC_MAX_N 70  /* vertices per batch */
#define VU_Q3STAGEC_ALIGN 4  /* batches start at multiples of it */
#define VU_Q3STAGEC_COUNTS 0  /* x vertices, z index quadwords, w indices */
#define VU_Q3STAGEC_IN_AT 1  /* first item */
#define VU_Q3STAGEC_OUT_AT 220  /* the output GIF tag */
#define VU_Q3STAGEC_NREG 3
#define VU_Q3STAGEC_REGS 0x512ull  /* GIF register list */
#define VU_Q3STAGEC_NIN 3
#define VU_Q3STAGEC_IN_POSITION 0  /* v4_32, 16 bytes */
#define VU_Q3STAGEC_IN_TEXCOORD 1  /* v2_32, 8 bytes */
#define VU_Q3STAGEC_IN_COLOR 2  /* v4_8u, 4 bytes */
#define VU_Q3STAGEC_INPUTS {{0xc, 0, 16, 0x0}, {0x4, 0, 8, 0x0}, {0xe, 1, 4, 0x0}}  /* UNPACK format, unsigned, bytes, STMASK (fields 1.0 from COL, values from ROW) */
#define VU_Q3STAGEC_ROW {0, 0, 0, 0}  /* ROW of the masked UNPACKs */

#endif
