/* Reference oracle: the A_CMD_ADPCM inner loop lifted VERBATIM from
 * src/static/jaudio_NES/internal/rspsim.c:120-211, TARGET_PC arm.
 * Only the surrounding plumbing (DMEM, command words) is replaced by argv/stdio;
 * every line inside the frame loop is byte-for-byte the shipped code.
 *
 * Reads from stdin:  n_frames, then per frame 9 bytes; then 8*16 book s16.
 * Writes to stdout:  n_frames*16 decoded s16, one per line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef short s16;
typedef int s32;
typedef unsigned char u8;
typedef unsigned short u16;

static s16 AD4[16] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0xFFF8, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFD, 0xFFFE, 0xFFFF,
};

static s16 ADPCM_BOOKBUF[8][16];

int main(void) {
    int n_frames, i;
    if (scanf("%d", &n_frames) != 1) return 1;

    u8 *in = malloc((size_t)n_frames * 9);
    for (i = 0; i < n_frames * 9; i++) { int v; scanf("%d", &v); in[i] = (u8)v; }
    for (i = 0; i < 8 * 16; i++) { int v; scanf("%d", &v); ADPCM_BOOKBUF[i / 16][i % 16] = (s16)v; }

    /* 16-sample history block, then the output area — rspsim's DMEM layout. */
    s16 *hist = calloc(16 + (size_t)n_frames * 16, sizeof(s16));

    s16 sp9C[16];
    s32 sp7C[8];
    s32 sp5C[8];

    /* ---- verbatim from rspsim.c:120-210 ---- */
    s16 *var_r17 = hist + 16;
    s16 var_r5 = var_r17[-1];
    s16 var_r0 = var_r17[-2];
    u8 *var_r18 = in;
    u16 sp13C = (u16)n_frames;
    s32 var_r12;
    s32 j, k, l;
    for (j = 0; j < sp13C; j++) {
        s16 temp_r4 = *var_r18++;
        s32 temp_r19 = temp_r4 >> 4;
        s16 *temp_r16 = ADPCM_BOOKBUF[temp_r4 & 0xF];
        for (k = 0; k < 8; k++) {
            s16 temp_r14_2 = *var_r18++;
            sp9C[k * 2 + 0] = AD4[(temp_r14_2 >> 4) & 0xF];
            sp9C[k * 2 + 1] = AD4[(temp_r14_2 >> 0) & 0xF];
        }
        for (k = 0; k < 8; k++) {
            sp7C[k] = sp9C[k] << temp_r19;
            {
                s32 accu = (s32)sp7C[k] << 11;
                accu += (s32)var_r5 * temp_r16[k + 8];
                accu += (s32)var_r0 * temp_r16[k];
                for (l = 0; l < k; l++) {
                    accu += (s32)sp7C[l] * temp_r16[k - l + 7];
                }
                var_r12 = accu >> 11;
            }
            if (var_r12 > 0x7FFF) var_r12 = 0x7FFF;
            if (var_r12 < -0x8000) var_r12 = -0x8000;
            sp9C[k] = var_r12;
        }
        var_r5 = sp9C[7];
        var_r0 = sp9C[6];
        for (k = 0; k < 8; k++) {
            sp5C[k] = sp9C[k + 8] << temp_r19;
            {
                s32 accu = (s32)sp5C[k] << 11;
                accu += (s32)var_r5 * temp_r16[k + 8];
                accu += (s32)var_r0 * temp_r16[k];
                for (l = 0; l < k; l++) {
                    accu += (s32)sp5C[l] * temp_r16[k - l + 7];
                }
                var_r12 = accu >> 11;
            }
            if (var_r12 > 0x7FFF) var_r12 = 0x7FFF;
            if (var_r12 < -0x8000) var_r12 = -0x8000;
            sp9C[k + 8] = var_r12;
        }
        var_r5 = sp9C[15];
        var_r0 = sp9C[14];
        for (k = 0; k < 16; k++) {
            *var_r17++ = sp9C[k];
        }
    }
    /* ---- end verbatim ---- */

    for (i = 0; i < n_frames * 16; i++) printf("%d\n", hist[16 + i]);
    return 0;
}
