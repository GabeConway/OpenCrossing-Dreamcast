/* dc_card.h - the VMU storage layer behind the GameCube CARD* API.
 *
 * Only three things need to be visible outside dc_card.c:
 *   - the container format constants, so a host tool can read/write the same
 *     bytes (tools/savebench/),
 *   - the raw save-blob entry points, so the save path can be exercised
 *     without going through the 20-byte CARDFileInfo dance, and
 *   - the boot self-test, which is what proves any of this works.
 *
 * The CARD* prototypes themselves deliberately stay out of here: the game gets
 * them from dolphin/card.h and the two declarations must not be able to drift.
 */
#ifndef DC_CARD_H
#define DC_CARD_H

/* --- on-VMU container -----------------------------------------------------
 * Payload handed to vmu_pkg_build(), i.e. what sits after the 640 B VMS
 * header. LITTLE-ENDIAN on purpose: this container never leaves the Dreamcast.
 * GCI interchange with Dolphin stays big-endian and stays the host tool's job
 * (kb/save-plan.md §4.6) — do not "fix" the endianness here to match the GCI.
 *
 *   off  size  field
 *     0     4  magic "OCS1"
 *     4     2  flags        bit0 = chunks are zlib deflate streams
 *     6     2  hdr_bytes    = DC_SAVE_HDR_BYTES (32)
 *     8     4  raw_len      uncompressed payload length
 *    12     4  stored_len   bytes of chunk data after the chunk table
 *    16     4  raw_crc      crc32 of the uncompressed payload
 *    20     4  stored_crc   crc32 of the chunk data
 *    24     2  chunk_log2   14 => 16 KB chunks; 0 => one stream
 *    26     2  chunk_cnt
 *    28     4  reserved (0)
 *    32  4*n   u32 stored length of each chunk
 *   ...        chunk data, back to back
 */
#define DC_SAVE_MAGIC0        'O'
#define DC_SAVE_MAGIC1        'C'
#define DC_SAVE_MAGIC2        'S'
#define DC_SAVE_MAGIC3        '1'
#define DC_SAVE_HDR_BYTES     32
#define DC_SAVE_FLAG_DEFLATE  0x0001

/* kb/save-plan.md §4.3: independent 16 KB chunks, so an unchanged chunk keeps
 * byte-identical output and a future incremental writer can skip it. Costs
 * +3.8% over a single stream (SYNTHETIC measurement — unverified). */
#define DC_SAVE_CHUNK_LOG2    14
#define DC_SAVE_CHUNK_BYTES   (1u << DC_SAVE_CHUNK_LOG2)

/* VMU filenames are 12 chars, no terminator (dc/vmufs.h vmu_dir_t.filename).
 * "DobutsunomoriP_MURA" does not fit, so the on-VMU name is ours and the GCI
 * name only survives in the host interchange tool. */
#define DC_SAVE_VMU_FILE      "OPENXING.SAV"
#define DC_SAVE_VMU_APPID     "OPENCROSSING"

/* --- result codes ---------------------------------------------------------
 * Distinct from the CARD_RESULT_* the game sees, because the failure modes a
 * console has (no VMU in the slot, VMU full, someone else's file under our
 * name) are not the same set the GameCube API models. dc_card.c maps them. */
typedef enum {
    DC_SAVE_OK        =  0,
    DC_SAVE_ENODEV    = -1,   /* no VMU in that slot — NORMAL, not an error   */
    DC_SAVE_ENOENT    = -2,   /* VMU present, our file is not on it           */
    DC_SAVE_ENOSPC    = -3,   /* would not fit; nothing was written           */
    DC_SAVE_EIO       = -4,   /* maple/flash failure                          */
    DC_SAVE_ECORRUPT  = -5,   /* VMS CRC or payload CRC mismatch              */
    DC_SAVE_EFOREIGN  = -6,   /* a real file, but not ours                    */
    DC_SAVE_ENOMEM    = -7,
    DC_SAVE_EDISABLED = -8    /* kill switch is on                            */
} dc_save_err_t;

const char* dc_save_strerror(int err);

/* Whole-blob store/load. `chan` is the CARD channel (0 = card A, 1 = card B),
 * mapped to the Nth VMU on the maple bus. */
int dc_save_store(int chan, const char* vmu_name, const unsigned char* src,
                  unsigned int len);
int dc_save_load(int chan, const char* vmu_name, unsigned char* dst,
                 unsigned int dst_cap, unsigned int* out_len);
int dc_save_erase(int chan, const char* vmu_name);
int dc_save_free_blocks(int chan);          /* <0 = dc_save_err_t */
int dc_save_present(int chan, const char* vmu_name);   /* 1 = our file is there */

/* Runs once. Writes a real block to a real VMU, reads it back, compares every
 * byte, and prints the result — that line is the only proof the save path
 * works. Safe to call with no VMU attached. */
void dc_card_selftest(void);

#endif /* DC_CARD_H */
