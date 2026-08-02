/* dc_card.c - GameCube memory card API on a Dreamcast VMU.
 *
 * Replaces pc/src/pc_card.c (one GCI file per card slot on disk). The 29 CARD*
 * entry points and the 20-byte CARDFileInfo layout are unchanged — the game
 * allocates those structs — only the backend moves.
 *
 * THE PROBLEM (PLAN §6, kb/save-budget.md):
 *   GCI_FILE_DATA_SIZE = mCD_LAND_SAVE_SIZE = 0x72000 = 466,944 B (~456 KB).
 *   VMU user space  = 200 x 512 B = 102,400 B.
 *   That is a 4.56x overshoot before compression. kb/save-plan.md §4 is the
 *   plan this file implements the storage half of: drop the GC banner, the
 *   sector padding and the backup copy (456 KB -> 289 KB of unique payload),
 *   then chunked deflate-6.
 *
 * WHAT IS IMPLEMENTED HERE, 2026-08-02:
 *   - a real vmufs backend: enumerate, read, write, delete, free-block count
 *   - the OCS1 container (dc/include/dc_card.h): chunked deflate-6 over zlib,
 *     crc32 on both the raw payload and the stored bytes, VMS header via
 *     vmu_pkg_build so the file shows up in the BIOS file manager
 *   - explicit handling of the three console failure modes: no VMU (normal),
 *     VMU full (refuse, never half-write), foreign/corrupt file (refuse)
 *   - dc_card_selftest(), which write/read/compares a real block on a real
 *     VMU and prints the result. That log line is the proof; nothing here is
 *     asserted without it.
 *
 * WHAT IS **NOT** IMPLEMENTED, and is the honest gap:
 *   pc/src/pc_m_card.c does its save I/O with <stdio.h> against the relative
 *   path "save/card_a/DobutsunomoriP_MURA.gci" — it does NOT route the town
 *   through CARDOpen/CARDRead/CARDWrite. Only CARDInit() and
 *   pc_card_scan_for_gci() are on its path. So making this file real makes the
 *   CARD API real; it does not by itself make the game load or save. The
 *   missing piece is a writable VFS at that path whose commit point flushes to
 *   the VMU. See kb/save-budget.md §7 (added 2026-08-02) for the design and
 *   why it is a separate change: it needs a KOS vfs_handler_t plus an
 *   fs_chdir() from dc_main.c, which belongs to another work stream.
 *
 * WHY THIS FILE STAGES IN RAM:
 *   CARDRead/CARDWrite are random-access at byte offsets. VMU flash is
 *   whole-file, block-granular, and slow. So the card file is staged in RAM and
 *   flushed to the VMU on CARDClose. That staging buffer is 466,944 B today,
 *   which is ITSELF over budget — it is sized by the same number compression
 *   has to shrink. Loudly reported at allocation time so it cannot be
 *   forgotten.
 *
 * WHAT IS DELIBERATELY NOT DONE HERE: pc/src/pc_save_bswap.c is REFERENCED
 * unchanged. SH-4 is little-endian exactly like the base targets, so the
 * on-disk image stays big-endian and GCI files keep interchanging with Dolphin
 * and the other ports. Do not "fix" the byte swapping for Dreamcast.
 *
 * KILL SWITCHES (CLAUDE.md: every optimization gets one, and anything that can
 * corrupt a save needs one more than most). All compile-time -D:
 *   DC_CARD_DISABLE=1     every entry point behaves as "no memory card".
 *   DC_CARD_READONLY=1    reads work, writes refuse. For bisecting a suspected
 *                         save-corrupting change without losing the save.
 *   DC_CARD_NO_COMPRESS=1 store chunks raw. The reader always handles both, so
 *                         a save written either way still loads.
 *   DC_CARD_NO_CHUNK=1    one deflate stream instead of 16 KB chunks.
 *   DC_CARD_SELFTEST=0    skip the boot self-test (it writes to the VMU).
 *   DC_CARD_BENCH=1       also run the deflate-throughput bench. OFF by
 *                         default: it wants ~600 KB transient on a machine
 *                         that is already 9 MB over budget (kb/STATE.md).
 */
#ifdef DC_CARD_STANDALONE
/* Standalone harness build (tools/savebench/dcvmu/). Gives the file
 * the handful of names dc_platform.h would have supplied, so the VMU layer the
 * harness proves is bit-for-bit the one the game links. Do not let this drift
 * into a second implementation. */
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned long      u32;
typedef unsigned long long u64;
typedef signed long        s32;
#define DC_LOGE(...)              printf(__VA_ARGS__)
#define DC_UNIMPLEMENTED_NOTE(n)  ((void)(n))
#define dc_mem_note(b, d)         ((void)(b), (void)(d))
#define DCMEM_DISC_IO             0
#else
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#endif

#include "dc_card.h"

#ifndef DC_HOST_STUB
#include <dc/maple.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <arch/timer.h>
#include <zlib/zlib.h>
#endif

#define CARD_RESULT_READY        0
#define CARD_RESULT_BUSY        -1
#define CARD_RESULT_WRONGDEVICE -2
#define CARD_RESULT_NOCARD      -3
#define CARD_RESULT_NOFILE      -4
#define CARD_RESULT_IOERROR     -5
#define CARD_RESULT_BROKEN      -6
#define CARD_RESULT_EXIST       -7
#define CARD_RESULT_NOENT       -8
#define CARD_RESULT_INSSPACE    -9
#define CARD_RESULT_NOPERM      -10
#define CARD_RESULT_LIMIT       -11
#define CARD_RESULT_NAMETOOLONG -12
#define CARD_RESULT_ENCODING    -13
#define CARD_RESULT_CANCELED    -14
#define CARD_RESULT_FATAL_ERROR -128

#define CARD_SECTOR_SIZE 8192

/* VMU geometry. 256 physical blocks of 512 B; 200 are user data, the rest is
 * root/FAT/directory (kb/save-budget.md §3). vmufs_free_blocks() reports the
 * real number for the card actually plugged in — these are only used for the
 * capacity the GameCube API insists on being told. */
#define VMU_BLOCK_BYTES   512
#define VMU_USER_BLOCKS   200
#define VMU_USER_BYTES    (VMU_USER_BLOCKS * VMU_BLOCK_BYTES)

/* Default OFF in the game build. The self-test writes a temp file to the
 * player's real VMU at CARDInit() time, which is not something a game may do
 * to somebody's memory card on every boot. tools/savebench/dcvmu builds the
 * same dc_card.c with -DDC_CARD_SELFTEST=1 and is where it belongs. */
#ifndef DC_CARD_SELFTEST
#define DC_CARD_SELFTEST 0
#endif

/* ==========================================================================
 * Little-endian pokes for the OCS1 container.
 * ==========================================================================
 * Written byte-wise rather than by struct overlay on purpose: the container is
 * defined by dc_card.h's byte table, and a host tool has to be able to produce
 * it without agreeing with SH-4 on struct padding. */
static void le32(u8* p, u32 v) {
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}
static void le16(u8* p, u32 v) {
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
}
static u32 rd32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 rd16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

const char* dc_save_strerror(int err) {
    switch (err) {
    case DC_SAVE_OK:        return "ok";
    case DC_SAVE_ENODEV:    return "no VMU in that slot";
    case DC_SAVE_ENOENT:    return "no save file on that VMU";
    case DC_SAVE_ENOSPC:    return "VMU full";
    case DC_SAVE_EIO:       return "maple/flash I/O error";
    case DC_SAVE_ECORRUPT:  return "corrupt (CRC mismatch)";
    case DC_SAVE_EFOREIGN:  return "file is not an OpenCrossing save";
    case DC_SAVE_ENOMEM:    return "out of memory";
    case DC_SAVE_EDISABLED: return "card layer disabled at compile time";
    default:                return "unknown";
    }
}

#ifndef DC_HOST_STUB

/* --- device lookup --------------------------------------------------------
 * maple_enum_type(n, MAPLE_FUNC_MEMCARD) is the Nth memory card on the whole
 * bus, not "port n". That is exactly what the CARD channel means to us:
 * channel 0 = the first VMU the player has plugged in anywhere, channel 1 =
 * the second. It keeps working when the player uses controller ports B/C/D,
 * which a port-indexed lookup would not.
 *
 * A missing VMU is NORMAL on a console. It must never be an error path that
 * crashes or retries — it is the "no memory card" flow the game already has. */
static maple_device_t* dc_vmu_dev(int chan) {
    if (chan < 0 || chan > 1) return NULL;
    return maple_enum_type(chan, MAPLE_FUNC_MEMCARD);
}

/* --- compression ----------------------------------------------------------
 * kb/save-plan.md §4.3: zlib deflate level 6, independent 16 KB chunks.
 * deflate-9 measured 0.6% better for ~4x the CPU and lzma 14% better for far
 * more of both, so 6 is the pick — but note that every one of those numbers is
 * SYNTHETIC (kb/save-budget.md's own warning) and none of them has met a real
 * .gci. Treat the ratio as unverified; the *format* does not depend on it.
 *
 * One z_stream reused with deflateReset() across chunks, not compress2() per
 * chunk: compress2 would build and tear down ~256 KB of deflate state 29 times
 * for a 467 KB payload, which is real churn on a 16 MB machine. */
#define DC_DEFLATE_LEVEL 6

static u32 dc_chunk_bytes(void) {
#ifdef DC_CARD_NO_CHUNK
    return 0;                       /* single stream */
#else
    return DC_SAVE_CHUNK_BYTES;
#endif
}

/* Upper bound on the whole container for a raw payload of `len`. */
static u32 dc_save_bound(u32 len) {
    u32 cb = dc_chunk_bytes();
    u32 n  = (cb == 0) ? 1 : ((len + cb - 1) / cb);
    if (n == 0) n = 1;
    /* compressBound() per chunk covers the incompressible case, where deflate
     * stores the block and adds a small header. Never assume compression makes
     * anything smaller — the adversarial save in kb/save-budget.md §2.1 makes
     * the design art EXPAND. */
    return (u32)DC_SAVE_HDR_BYTES + 4u * n +
           (u32)compressBound((uLong)len) + 64u * n;
}

/* Build the OCS1 container. Returns bytes written, or <0 (dc_save_err_t). */
static int dc_save_pack(const u8* src, u32 len, u8* out, u32 out_cap) {
    u32 cb   = dc_chunk_bytes();
    u32 n    = (cb == 0) ? 1 : ((len + cb - 1) / cb);
    u32 tbl, data_off, off, i;
    u16 flags = 0;
#ifndef DC_CARD_NO_COMPRESS
    z_stream zs;
    int zrc;
    flags |= DC_SAVE_FLAG_DEFLATE;
#endif
    if (n == 0) n = 1;
    tbl      = DC_SAVE_HDR_BYTES;
    data_off = tbl + 4u * n;
    if (out_cap < data_off) return DC_SAVE_ENOMEM;

    off = data_off;

#ifndef DC_CARD_NO_COMPRESS
    memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, DC_DEFLATE_LEVEL) != Z_OK) return DC_SAVE_ENOMEM;
#endif

    for (i = 0; i < n; i++) {
        u32 c_off = (cb == 0) ? 0 : i * cb;
        u32 c_len = (cb == 0) ? len : (len - c_off < cb ? len - c_off : cb);
        u32 wrote;
#ifdef DC_CARD_NO_COMPRESS
        if (off + c_len > out_cap) return DC_SAVE_ENOMEM;
        memcpy(out + off, src + c_off, c_len);
        wrote = c_len;
#else
        zs.next_in   = (Bytef*)(src + c_off);
        zs.avail_in  = (uInt)c_len;
        zs.next_out  = (Bytef*)(out + off);
        zs.avail_out = (uInt)(out_cap - off);
        zrc = deflate(&zs, Z_FINISH);
        if (zrc != Z_STREAM_END) { deflateEnd(&zs); return DC_SAVE_ENOMEM; }
        wrote = (u32)((u8*)zs.next_out - (out + off));
        if (deflateReset(&zs) != Z_OK) { deflateEnd(&zs); return DC_SAVE_ENOMEM; }
#endif
        le32(out + tbl + 4u * i, wrote);
        off += wrote;
    }

#ifndef DC_CARD_NO_COMPRESS
    deflateEnd(&zs);
#endif

    out[0] = DC_SAVE_MAGIC0; out[1] = DC_SAVE_MAGIC1;
    out[2] = DC_SAVE_MAGIC2; out[3] = DC_SAVE_MAGIC3;
    le16(out + 4,  flags);
    le16(out + 6,  DC_SAVE_HDR_BYTES);
    le32(out + 8,  len);
    le32(out + 12, off - data_off);
    le32(out + 16, (u32)crc32(0, (const Bytef*)src, (uInt)len));
    le32(out + 20, (u32)crc32(0, (const Bytef*)(out + data_off),
                              (uInt)(off - data_off)));
    le16(out + 24, (cb == 0) ? 0 : DC_SAVE_CHUNK_LOG2);
    le16(out + 26, n);
    le32(out + 28, 0);
    return (int)off;
}

/* Unpack an OCS1 container. Every rejection reason is distinct so the log says
 * WHY a save did not load — "corrupt" and "someone else's file" are different
 * bug reports. */
static int dc_save_unpack(const u8* in, u32 in_len, u8* dst, u32 dst_cap,
                          u32* out_len) {
    u32 flags, hdr, raw_len, stored_len, raw_crc, stored_crc, cl2, n, cb;
    u32 tbl, data_off, off, i, dst_off;

    if (in_len < DC_SAVE_HDR_BYTES) return DC_SAVE_EFOREIGN;
    if (in[0] != DC_SAVE_MAGIC0 || in[1] != DC_SAVE_MAGIC1 ||
        in[2] != DC_SAVE_MAGIC2 || in[3] != DC_SAVE_MAGIC3)
        return DC_SAVE_EFOREIGN;

    flags      = rd16(in + 4);
    hdr        = rd16(in + 6);
    raw_len    = rd32(in + 8);
    stored_len = rd32(in + 12);
    raw_crc    = rd32(in + 16);
    stored_crc = rd32(in + 20);
    cl2        = rd16(in + 24);
    n          = rd16(in + 26);

    if (hdr != DC_SAVE_HDR_BYTES || n == 0) return DC_SAVE_ECORRUPT;
    tbl      = hdr;
    data_off = tbl + 4u * n;
    if (data_off > in_len || stored_len > in_len - data_off)
        return DC_SAVE_ECORRUPT;
    if ((u32)crc32(0, (const Bytef*)(in + data_off), (uInt)stored_len) != stored_crc)
        return DC_SAVE_ECORRUPT;
    if (raw_len > dst_cap) return DC_SAVE_ENOMEM;

    cb = (cl2 == 0) ? raw_len : (1u << cl2);
    off = data_off;
    dst_off = 0;
    for (i = 0; i < n; i++) {
        u32 c_stored = rd32(in + tbl + 4u * i);
        u32 c_raw    = (raw_len - dst_off < cb) ? (raw_len - dst_off) : cb;
        if (off + c_stored > in_len || dst_off + c_raw > dst_cap)
            return DC_SAVE_ECORRUPT;
        if (flags & DC_SAVE_FLAG_DEFLATE) {
            uLongf got = (uLongf)c_raw;
            if (uncompress((Bytef*)(dst + dst_off), &got,
                           (const Bytef*)(in + off), (uLong)c_stored) != Z_OK ||
                (u32)got != c_raw)
                return DC_SAVE_ECORRUPT;
        } else {
            if (c_stored != c_raw) return DC_SAVE_ECORRUPT;
            memcpy(dst + dst_off, in + off, c_raw);
        }
        off     += c_stored;
        dst_off += c_raw;
    }
    if (dst_off != raw_len) return DC_SAVE_ECORRUPT;
    if ((u32)crc32(0, (const Bytef*)dst, (uInt)raw_len) != raw_crc)
        return DC_SAVE_ECORRUPT;
    if (out_len) *out_len = raw_len;
    return DC_SAVE_OK;
}

/* --- VMS icon -------------------------------------------------------------
 * Built on the stack, not linked as .data: 512 B is noise next to the 9 MB the
 * image is over, but a rule that says "no new resident bytes unless they earn
 * it" is only a rule if it holds for the small ones too. A file with icon_cnt
 * 0 is legal but the BIOS file manager shows nothing for it, so the player
 * cannot recognise or copy their own save — worth the 512 B on the card. */
static void dc_vmu_icon(u16* pal, u8* px) {
    int y, x;
    memset(pal, 0, 16 * sizeof(u16));
    pal[0] = 0xF264;   /* ARGB4444 leaf green   */
    pal[1] = 0xFEC8;   /* ARGB4444 sand         */
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            int dx = x - 16, dy = y - 16;
            int c = (dx * dx + dy * dy) < 150 ? 0 : 1;
            u8* b = px + (y * 32 + x) / 2;
            if (x & 1) *b = (u8)((*b & 0xF0) | (u8)c);
            else       *b = (u8)((*b & 0x0F) | (u8)(c << 4));
        }
    }
}

/* --- store ----------------------------------------------------------------
 * Order matters and is the whole safety story:
 *   1. pack first, into RAM (dc_save_store_dev). A pack failure must not have
 *      touched the card.
 *   2. count the space the packed blob needs, ADDING BACK the blocks our own
 *      existing file already occupies — vmufs_write(VMUFS_OVERWRITE) frees
 *      them before it allocates. Without that, re-saving a town onto a
 *      nearly-full VMU reports "full" when it would in fact have fit.
 *   3. refuse *before* writing if it does not fit. Never delete the old save
 *      and then discover there is no room for the new one.
 *
 * dc_vmu_put() is step 2+3: wrap an already-final payload in a VMS header and
 * put it on the card. Split out from dc_save_store_dev() so the self-test can
 * put a file with a FOREIGN app_id, or a deliberately mangled payload, on the
 * card and prove the reader rejects it — a rejection path nobody exercises is
 * a rejection path nobody has.
 */
static int dc_vmu_put(maple_device_t* dev, const char* name, const char* appid,
                      const u8* payload, u32 payload_len, u32* out_blocks,
                      u32* out_packed) {
#ifdef DC_CARD_READONLY
    (void)dev; (void)name; (void)appid; (void)payload; (void)payload_len;
    (void)out_blocks; (void)out_packed;
    DC_LOGE("[DC/CARD] DC_CARD_READONLY=1 — refusing to write\n");
    return DC_SAVE_EDISABLED;
#else
    u8* pkgbuf = NULL;
    u8* vms = NULL;
    int pkgsize = 0;
    int rc;
    u32 need_blocks, have_blocks;
    vmu_pkg_t pkg;
    u16 icon_pal[16];
    u8  icon_px[512];

    if (!dev) return DC_SAVE_ENODEV;

    memset(icon_px, 0, sizeof(icon_px));
    dc_vmu_icon(icon_pal, icon_px);

    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "Animal Crossing", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long,  "OpenCrossing town data", sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id,     appid, sizeof(pkg.app_id) - 1);
    pkg.icon_cnt        = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type   = VMUPKG_EC_NONE;   /* an eyecatch costs 2-8 KB of a
                                             * 100 KB card. Never ship one. */
    pkg.data_len        = (int)payload_len;
    memcpy(pkg.icon_pal, icon_pal, sizeof(icon_pal));
    pkg.icon_data       = icon_px;
    pkg.eyecatch_data   = NULL;
    pkg.data            = payload;

    if (vmu_pkg_build(&pkg, &pkgbuf, &pkgsize) < 0 || !pkgbuf)
        return DC_SAVE_EIO;
    vms = pkgbuf;

    need_blocks = ((u32)pkgsize + VMU_BLOCK_BYTES - 1) / VMU_BLOCK_BYTES;

    /* free + what our own file would give back */
    {
        int freeb = vmufs_free_blocks(dev);
        vmu_dir_t* dir = NULL;
        int cnt = 0, i;
        have_blocks = (freeb > 0) ? (u32)freeb : 0;
        if (vmufs_readdir(dev, &dir, &cnt) == 0 && dir) {
            for (i = 0; i < cnt; i++) {
                if (!strncmp(dir[i].filename, name, 12)) {
                    have_blocks += dir[i].filesize;
                    break;
                }
            }
            free(dir);
        }
    }

    if (need_blocks > have_blocks) {
        DC_LOGE("[DC/CARD] VMU FULL: save needs %u blocks, %u available. "
                "Nothing written; the existing save is untouched.\n",
                (unsigned)need_blocks, (unsigned)have_blocks);
        free(vms);
        return DC_SAVE_ENOSPC;
    }

    if (vmufs_write(dev, name, vms, pkgsize, VMUFS_OVERWRITE) == 0)
        rc = DC_SAVE_OK;
    else
        rc = DC_SAVE_EIO;

    if (out_blocks) *out_blocks = need_blocks;
    if (out_packed) *out_packed = (u32)pkgsize;
    free(vms);
    return rc;
#endif
}

/* Pack, then put. Packing happens first and entirely in RAM: a pack failure
 * must never have touched the card. */
static int dc_save_store_dev(maple_device_t* dev, const char* name,
                             const u8* src, u32 len, u32* out_blocks,
                             u32* out_packed) {
    u32 cap;
    u8* packed;
    int packed_len, rc;

    if (!dev) return DC_SAVE_ENODEV;
    cap = dc_save_bound(len);
    packed = (u8*)malloc(cap);
    if (!packed) return DC_SAVE_ENOMEM;
    packed_len = dc_save_pack(src, len, packed, cap);
    if (packed_len < 0) { free(packed); return packed_len; }
    rc = dc_vmu_put(dev, name, DC_SAVE_VMU_APPID, packed, (u32)packed_len,
                    out_blocks, out_packed);
    free(packed);
    return rc;
}

int dc_save_store(int chan, const char* vmu_name, const unsigned char* src,
                  unsigned int len) {
#ifdef DC_CARD_DISABLE
    (void)chan; (void)vmu_name; (void)src; (void)len;
    return DC_SAVE_EDISABLED;
#else
    maple_device_t* dev = dc_vmu_dev(chan);
    const char* name = vmu_name ? vmu_name : DC_SAVE_VMU_FILE;
    u32 blocks = 0, packed = 0;
    int rc;
    if (!dev) return DC_SAVE_ENODEV;
    rc = dc_save_store_dev(dev, name, (const u8*)src, (u32)len, &blocks, &packed);
    if (rc == DC_SAVE_OK) {
        DC_LOGE("[DC/CARD] wrote %s: %u raw -> %u on card (%u blocks, %u%% of "
                "a %u-block VMU)\n",
                name, (unsigned)len, (unsigned)packed, (unsigned)blocks,
                (unsigned)((blocks * 100u) / VMU_USER_BLOCKS),
                (unsigned)VMU_USER_BLOCKS);
    }
    return rc;
#endif
}

int dc_save_load(int chan, const char* vmu_name, unsigned char* dst,
                 unsigned int dst_cap, unsigned int* out_len) {
#ifdef DC_CARD_DISABLE
    (void)chan; (void)vmu_name; (void)dst; (void)dst_cap; (void)out_len;
    return DC_SAVE_EDISABLED;
#else
    maple_device_t* dev = dc_vmu_dev(chan);
    void* raw = NULL;
    int rawsize = 0;
    vmu_pkg_t pkg;
    u32 got = 0;
    int rc;

    if (!dev) return DC_SAVE_ENODEV;
    if (vmufs_read(dev, vmu_name ? vmu_name : DC_SAVE_VMU_FILE,
                   &raw, &rawsize) < 0 || !raw)
        return DC_SAVE_ENOENT;

    /* vmu_pkg_parse returns -1 on a bad VMS CRC. That is the first of the two
     * integrity gates; the OCS1 payload crc32 in dc_save_unpack is the second.
     * Both exist because we cannot afford the GameCube's second copy of the
     * save (kb/save-plan.md §4.1) — detection replaces redundancy. */
    memset(&pkg, 0, sizeof(pkg));
    if (vmu_pkg_parse((uint8_t*)raw, (size_t)rawsize, &pkg) < 0) {
        free(raw);
        DC_LOGE("[DC/CARD] %s: VMS header CRC bad — refusing to load\n",
                vmu_name ? vmu_name : DC_SAVE_VMU_FILE);
        return DC_SAVE_ECORRUPT;
    }
    if (strncmp(pkg.app_id, DC_SAVE_VMU_APPID, strlen(DC_SAVE_VMU_APPID)) != 0) {
        free(raw);
        DC_LOGE("[DC/CARD] %s: app_id '%.16s' is not ours — leaving it alone\n",
                vmu_name ? vmu_name : DC_SAVE_VMU_FILE, pkg.app_id);
        return DC_SAVE_EFOREIGN;
    }

    rc = dc_save_unpack((const u8*)pkg.data, (u32)pkg.data_len,
                        (u8*)dst, (u32)dst_cap, &got);
    free(raw);
    if (rc != DC_SAVE_OK) {
        DC_LOGE("[DC/CARD] %s: %s\n", vmu_name ? vmu_name : DC_SAVE_VMU_FILE,
                dc_save_strerror(rc));
        return rc;
    }
    if (out_len) *out_len = got;
    return DC_SAVE_OK;
#endif
}

int dc_save_erase(int chan, const char* vmu_name) {
#if defined(DC_CARD_DISABLE) || defined(DC_CARD_READONLY)
    (void)chan; (void)vmu_name;
    return DC_SAVE_EDISABLED;
#else
    maple_device_t* dev = dc_vmu_dev(chan);
    if (!dev) return DC_SAVE_ENODEV;
    return vmufs_delete(dev, vmu_name ? vmu_name : DC_SAVE_VMU_FILE) == 0
           ? DC_SAVE_OK : DC_SAVE_ENOENT;
#endif
}

int dc_save_free_blocks(int chan) {
    maple_device_t* dev = dc_vmu_dev(chan);
    int n;
    if (!dev) return DC_SAVE_ENODEV;
    n = vmufs_free_blocks(dev);
    return (n < 0) ? DC_SAVE_EIO : n;
}

int dc_save_present(int chan, const char* vmu_name) {
    maple_device_t* dev = dc_vmu_dev(chan);
    vmu_dir_t* dir = NULL;
    int cnt = 0, i, found = 0;
    const char* name = vmu_name ? vmu_name : DC_SAVE_VMU_FILE;
    if (!dev) return 0;
    if (vmufs_readdir(dev, &dir, &cnt) < 0 || !dir) return 0;
    for (i = 0; i < cnt; i++) {
        if (!strncmp(dir[i].filename, name, 12)) { found = 1; break; }
    }
    free(dir);
    return found;
}

#else  /* DC_HOST_STUB — host syntax check only, no maple bus */

int dc_save_store(int c, const char* n, const unsigned char* s, unsigned int l)
{ (void)c; (void)n; (void)s; (void)l; return DC_SAVE_ENODEV; }
int dc_save_load(int c, const char* n, unsigned char* d, unsigned int cap,
                 unsigned int* o)
{ (void)c; (void)n; (void)d; (void)cap; (void)o; return DC_SAVE_ENODEV; }
int dc_save_erase(int c, const char* n) { (void)c; (void)n; return DC_SAVE_ENODEV; }
int dc_save_free_blocks(int c) { (void)c; return DC_SAVE_ENODEV; }
int dc_save_present(int c, const char* n) { (void)c; (void)n; return 0; }

#endif /* DC_HOST_STUB */

/* ==========================================================================
 * Self-test — the only thing that turns "implemented" into "proven"
 * ==========================================================================
 * Writes a real file to a real VMU, reads it back, compares every byte. Runs
 * once, from whichever of CARDInit()/pc_card_scan_for_gci() the game reaches
 * first. Prints one PASS/FAIL line plus the numbers kb/save-budget.md §6 lists
 * as open (per-block write cost, deflate throughput).
 *
 * SAFETY: it writes to OXTEST.TMP — a different name from the real save — and
 * deletes it afterwards. It must never be able to destroy a town.
 *
 * ⚠ Timing printed under Flycast is EMULATOR timing. MEASURED 2026-08-02
 * (kb/save-budget.md §7.4): Flycast reports 84.6 ms/block, which matches the
 * pessimistic bound derived from KOS's vmufs.c (5 dependent maple
 * transactions × the ~16.7 ms poll cadence = 83.5 ms) to within 1.3%. So it
 * models the maple cadence — but not flash-write latency, which means a real
 * VMU can only be slower than this, never faster. Only a real VMU settles it.
 */
#define DC_SELFTEST_FILE "OXTEST.TMP"

#ifndef DC_HOST_STUB
static void dc_selftest_fill(u8* p, u32 n, u32 seed) {
    /* Three regimes in one buffer, because a save is all three and a test
     * against only one of them proves the wrong thing:
     *   - a run of zeros (sector padding, empty slots) — compresses to nothing
     *   - low-entropy structured text (letters, diary)
     *   - a PRNG block (design art at its worst) — must survive byte-exact */
    u32 i;
    u32 s = seed ? seed : 1;
    for (i = 0; i < n; i++) {
        if (i < n / 3)          p[i] = 0;
        else if (i < 2 * n / 3) p[i] = (u8)('a' + (i % 26));
        else {
            s = s * 1103515245u + 12345u;
            p[i] = (u8)(s >> 16);
        }
    }
}

static u64 dc_now_us(void) { return timer_us_gettime64(); }
#endif

void dc_card_selftest(void) {
#if !DC_CARD_SELFTEST || defined(DC_HOST_STUB) || defined(DC_CARD_DISABLE)
    return;
#else
    static int done = 0;
    /* One GameCube memory-card sector (mCD_MEMCARD_SECTORSIZE). Deliberately
     * smaller than one 16 KB chunk, so the simplest path — a single chunk —
     * is what gets proven first. The multi-chunk path is covered by
     * tools/savebench/dcvmu/'s larger round-trips and by DC_CARD_BENCH. */
    const u32 n = CARD_SECTOR_SIZE;
    u8 *src = NULL, *back = NULL;
    maple_device_t* dev;
    int i, nvmu = 0;
    u64 t0, t_write = 0, t_read = 0;
    u32 blocks = 0, packed = 0;
    int rc;

    if (done) return;
    done = 1;

    for (i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD)) != NULL; i++) {
        int fb = vmufs_free_blocks(dev);
        DC_LOGE("[DC/CARD] VMU %d at %c%d '%.16s'  %d free blocks\n",
                i, 'A' + dev->port, dev->unit, dev->info.product_name, fb);
        nvmu++;
    }
    if (nvmu == 0) {
        /* Not a failure. A Dreamcast with no memory card is a Dreamcast. */
        DC_LOGE("[DC/CARD] SELFTEST skipped: no VMU present (this is normal)\n");
        return;
    }

    src  = (u8*)malloc(n);
    back = (u8*)malloc(n);
    if (!src || !back) {
        DC_LOGE("[DC/CARD] SELFTEST skipped: malloc(%u x2) failed\n", (unsigned)n);
        free(src); free(back);
        return;
    }
    dc_selftest_fill(src, n, 0xACAC1234u);
    memset(back, 0xA5, n);

    dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    t0 = dc_now_us();
    rc = dc_save_store_dev(dev, DC_SELFTEST_FILE, src, n, &blocks, &packed);
    t_write = dc_now_us() - t0;

    if (rc != DC_SAVE_OK) {
        DC_LOGE("[DC/CARD] SELFTEST FAIL: write: %s (%d)\n",
                dc_save_strerror(rc), rc);
        free(src); free(back);
        return;
    }

    t0 = dc_now_us();
    rc = dc_save_load(0, DC_SELFTEST_FILE, back, n, NULL);
    t_read = dc_now_us() - t0;

    if (rc != DC_SAVE_OK) {
        DC_LOGE("[DC/CARD] SELFTEST FAIL: read: %s (%d)\n",
                dc_save_strerror(rc), rc);
    } else if (memcmp(src, back, n) != 0) {
        u32 k, bad = 0, first = 0;
        for (k = 0; k < n; k++)
            if (src[k] != back[k]) { if (!bad) first = k; bad++; }
        DC_LOGE("[DC/CARD] SELFTEST FAIL: %u/%u bytes differ, first at %u\n",
                (unsigned)bad, (unsigned)n, (unsigned)first);
    } else {
        DC_LOGE("[DC/CARD] SELFTEST PASS: %u B round-tripped byte-exact "
                "(packed %u B = %u blocks, %u%% of a VMU)\n",
                (unsigned)n, (unsigned)packed, (unsigned)blocks,
                (unsigned)((blocks * 100u) / VMU_USER_BLOCKS));
        DC_LOGE("[DC/CARD] SELFTEST timing (EMULATOR, not hardware): "
                "write %u us (%u us/block), read %u us\n",
                (unsigned)t_write,
                (unsigned)(blocks ? t_write / blocks : 0),
                (unsigned)t_read);
    }

    /* --- the rejection paths ---------------------------------------------
     * A save layer that only ever meets its own good data is untested where it
     * matters. Each of these puts a real file on a real card (or removes one)
     * and demands a specific refusal — silence, or a "success", is a bug that
     * would hand the game a corrupt town. */
    {
        u8 small[256];
        u8 back2[256];
        u32 cap = dc_save_bound(sizeof(small));
        u8* p = (u8*)malloc(cap);
        int plen;

        dc_selftest_fill(small, sizeof(small), 0x1111u);
        if (p && (plen = dc_save_pack(small, sizeof(small), p, cap)) > 0) {
            /* (a) FOREIGN: valid VMS header and CRC, someone else's app id. */
            if (dc_vmu_put(dev, DC_SELFTEST_FILE, "SOMEOTHERGAME", p,
                           (u32)plen, NULL, NULL) == DC_SAVE_OK) {
                rc = dc_save_load(0, DC_SELFTEST_FILE, back2, sizeof(back2), NULL);
                DC_LOGE("[DC/CARD] SELFTEST foreign-file: rc=%d (%s) — %s\n",
                        rc, dc_save_strerror(rc),
                        rc == DC_SAVE_EFOREIGN ? "PASS" : "FAIL, expected -6");
            }
            /* (b) CORRUPT: our app id, but one byte of the payload flipped.
             * The VMS CRC is recomputed over the damaged bytes, so this gets
             * past vmu_pkg_parse and can only be caught by the OCS1 crc32 —
             * exactly the gate that replaces the GameCube's backup copy. */
            p[plen - 1] ^= 0xFF;
            if (dc_vmu_put(dev, DC_SELFTEST_FILE, DC_SAVE_VMU_APPID, p,
                           (u32)plen, NULL, NULL) == DC_SAVE_OK) {
                rc = dc_save_load(0, DC_SELFTEST_FILE, back2, sizeof(back2), NULL);
                DC_LOGE("[DC/CARD] SELFTEST corrupt-file: rc=%d (%s) — %s\n",
                        rc, dc_save_strerror(rc),
                        rc == DC_SAVE_ECORRUPT ? "PASS" : "FAIL, expected -5");
            }
        }
        free(p);

        /* (c) NO SUCH FILE, on a card that is present. */
        rc = dc_save_load(0, "OXNOPE.TMP", back2, sizeof(back2), NULL);
        DC_LOGE("[DC/CARD] SELFTEST missing-file: rc=%d (%s) — %s\n",
                rc, dc_save_strerror(rc),
                rc == DC_SAVE_ENOENT ? "PASS" : "FAIL, expected -2");

        /* (d) NO VMU: channel 1 is empty in most setups; when it is, prove the
         * layer reports ENODEV rather than faulting. A missing card is normal. */
        if (!dc_vmu_dev(1)) {
            rc = dc_save_load(1, DC_SAVE_VMU_FILE, back2, sizeof(back2), NULL);
            DC_LOGE("[DC/CARD] SELFTEST no-vmu(chan1): rc=%d (%s) — %s\n",
                    rc, dc_save_strerror(rc),
                    rc == DC_SAVE_ENODEV ? "PASS" : "FAIL, expected -1");
        }
    }

    /* (e) VMU FULL. Only in the standalone harness: it needs ~350 KB of
     * transient RAM, and the game build is already 9 MB over budget
     * (kb/STATE.md), so spending that at boot to test an error path is the
     * wrong trade there. The arithmetic under test is the same either way.
     * 112,640 B of PRNG output is incompressible, so it packs to more than the
     * 102,400 B a VMU holds and MUST be refused without touching the card. */
#ifdef DC_CARD_STANDALONE
    {
        const u32 toobig = 110u * 1024u;
        u8* big = (u8*)malloc(toobig);
        if (big) {
            int before = vmufs_free_blocks(dev);
            u32 k, s = 0xB0BAu;
            /* every byte high-entropy, not dc_selftest_fill's mixed profile:
             * that one's zero third would compress back under the limit */
            for (k = 0; k < toobig; k++) {
                s = s * 1103515245u + 12345u;
                big[k] = (u8)(s >> 16);
            }
            rc = dc_save_store_dev(dev, DC_SELFTEST_FILE, big, toobig, NULL, NULL);
            DC_LOGE("[DC/CARD] SELFTEST vmu-full: rc=%d (%s), free %d -> %d — %s\n",
                    rc, dc_save_strerror(rc), before, vmufs_free_blocks(dev),
                    (rc == DC_SAVE_ENOSPC && before == vmufs_free_blocks(dev))
                        ? "PASS (nothing written)" : "FAIL, expected -3 and no change");
            free(big);
        }
    }
#endif

    if (dc_save_erase(0, DC_SELFTEST_FILE) != DC_SAVE_OK)
        DC_LOGE("[DC/CARD] SELFTEST: could not delete %s — it is 1-2 blocks, "
                "delete it from the BIOS if it bothers you\n", DC_SELFTEST_FILE);

    free(src);
    free(back);

#ifdef DC_CARD_BENCH
    /* kb/save-plan.md §6.3: "measure deflate-6 throughput on SH-4". This is
     * that measurement, on save-SHAPED but SYNTHETIC content — it settles the
     * 40-100x hand-wave in the time column, and settles nothing at all about
     * the ratio, which depends on real player data nobody has. */
    {
        const u32 payload = 295910u;    /* kb/save-budget.md §1 unique payload */
        u8* big = (u8*)malloc(payload);
        u8* out = NULL;
        if (!big) {
            DC_LOGE("[DC/CARD] BENCH skipped: malloc(%u) failed\n",
                    (unsigned)payload);
        } else {
            u32 cap = dc_save_bound(payload);
            out = (u8*)malloc(cap);
            if (!out) {
                DC_LOGE("[DC/CARD] BENCH skipped: malloc(%u) failed\n",
                        (unsigned)cap);
            } else {
                int len;
                dc_selftest_fill(big, payload, 0x5EEDu);
                t0 = dc_now_us();
                len = dc_save_pack(big, payload, out, cap);
                t_write = dc_now_us() - t0;
                DC_LOGE("[DC/CARD] BENCH deflate-%d %u B -> %d B in %u us "
                        "(%u KB/s) => %u VMU blocks\n",
                        DC_DEFLATE_LEVEL, (unsigned)payload, len,
                        (unsigned)t_write,
                        (unsigned)(t_write ? (payload * 1000u) / (u32)t_write : 0),
                        (unsigned)(len > 0 ? ((u32)len + 639u + 511u) / 512u : 0));
                free(out);
            }
            free(big);
        }
    }
#endif
#endif
}

#ifndef DC_CARD_STANDALONE
/* ==========================================================================
 * CARD API
 * ==========================================================================
 * Note what these are and are not: the game's town save does NOT come through
 * here (see the file header). These exist because src/ and pc_m_card.c link
 * against them and because the CARD contract is the right shape for anything
 * that does adopt it. They are now backed by the real VMU, not a stub.
 */

/* Must match card.h exactly (20 bytes). Extra state lives in the side table. */
typedef struct {
    s32 chan;
    s32 fileNo;
    s32 offset;
    s32 length;
    u16 iBlock;
} CARDFileInfo_DC;

#define CARD_MAX_OPEN 4

typedef struct {
    CARDFileInfo_DC* owner;    /* NULL = slot free */
    u8*  staging;              /* RAM image of the card file */
    u32  staging_size;
    int  dirty;
    char filename[16];         /* the 12-char VMU name, terminated */
} CARDOpenSlot;

static CARDOpenSlot card_slots[CARD_MAX_OPEN];
static int card_mounted[2] = { 0, 0 };

/* GameCube file names are up to 32 chars; VMU dirents are 12 with no
 * terminator. Rather than silently truncating (two different GC files could
 * collapse onto one VMU name and eat each other), map the one name the game
 * actually uses and refuse anything else by name-too-long. */
static int dc_card_vmu_name(const char* gc_name, char* out12) {
    if (!gc_name || !gc_name[0]) return 0;
    if (strstr(gc_name, "..") || strchr(gc_name, '/') || strchr(gc_name, '\\'))
        return 0;
    if (!strncmp(gc_name, "DobutsunomoriP", 14)) {
        strcpy(out12, DC_SAVE_VMU_FILE);
        return 1;
    }
    if (strlen(gc_name) <= 12) { strcpy(out12, gc_name); return 1; }
    DC_LOGE("[DC/CARD] '%s' has no 12-char VMU name — refusing rather than "
            "truncating into a collision\n", gc_name);
    return 0;
}

static CARDOpenSlot* dc_card_slot_alloc(CARDFileInfo_DC* fi) {
    int i;
    for (i = 0; i < CARD_MAX_OPEN; i++) {
        if (card_slots[i].owner == NULL) {
            card_slots[i].owner = fi;
            card_slots[i].staging = NULL;
            card_slots[i].staging_size = 0;
            card_slots[i].dirty = 0;
            card_slots[i].filename[0] = '\0';
            return &card_slots[i];
        }
    }
    return NULL;
}

static CARDOpenSlot* dc_card_slot_find(CARDFileInfo_DC* fi) {
    int i;
    for (i = 0; i < CARD_MAX_OPEN; i++)
        if (card_slots[i].owner == fi) return &card_slots[i];
    return NULL;
}

static void dc_card_slot_free(CARDOpenSlot* slot) {
    if (!slot) return;
    if (slot->staging) {
        dc_mem_note(DCMEM_DISC_IO, -(ptrdiff_t)slot->staging_size);
        free(slot->staging);
        slot->staging = NULL;
    }
    slot->staging_size = 0;
    slot->owner = NULL;
}

static u8* dc_card_staging_alloc(u32 size) {
    u8* p = (u8*)malloc(size);
    if (!p) {
        DC_LOGE("[DC/CARD] staging alloc FAILED (%u B)\n", (unsigned)size);
        return NULL;
    }
    memset(p, 0, size);
    dc_mem_note(DCMEM_DISC_IO, (ptrdiff_t)size);
    if (size > VMU_USER_BYTES) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            DC_LOGE("[DC/CARD] WARNING: %u B card image staged in RAM; VMU user "
                    "space is %u B. It only reaches the card if it compresses "
                    "past 4.56:1 (kb/save-plan.md §4).\n",
                    (unsigned)size, (unsigned)VMU_USER_BYTES);
        }
    }
    return p;
}

void CARDInit(void) {
    memset(card_slots, 0, sizeof(card_slots));
    DC_LOGE("[DC/CARD] vmufs backend, on-card file '%s', app id '%s'\n",
            DC_SAVE_VMU_FILE, DC_SAVE_VMU_APPID);
    dc_card_selftest();
}

s32 CARDMount(s32 chan, void* workArea, void* detachCallback) {
    (void)workArea; (void)detachCallback;
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
#ifndef DC_HOST_STUB
    /* A missing VMU must report NOCARD so the game shows its own "no memory
     * card" flow instead of failing later inside a save. */
    if (!dc_vmu_dev((int)chan)) {
        DC_LOGE("[DC/CARD] no VMU for channel %d\n", (int)chan);
        card_mounted[chan] = 0;
        return CARD_RESULT_NOCARD;
    }
#endif
    card_mounted[chan] = 1;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, void* detachCb, void* attachCb) {
    s32 result = CARDMount(chan, workArea, detachCb);
    if (attachCb) ((void (*)(s32, s32))attachCb)(chan, result);
    return result;
}

s32 CARDUnmount(s32 chan) {
    if (chan >= 0 && chan <= 1) card_mounted[chan] = 0;
    return CARD_RESULT_READY;
}

/* Map a dc_save_err_t onto the GameCube result the game knows how to react to.
 * The mapping is where "a console has no memory card" stops being an error. */
static s32 dc_card_map_err(int e) {
    switch (e) {
    case DC_SAVE_OK:        return CARD_RESULT_READY;
    case DC_SAVE_ENODEV:    return CARD_RESULT_NOCARD;
    case DC_SAVE_ENOENT:    return CARD_RESULT_NOFILE;
    case DC_SAVE_ENOSPC:    return CARD_RESULT_INSSPACE;
    case DC_SAVE_ECORRUPT:  return CARD_RESULT_BROKEN;
    case DC_SAVE_EFOREIGN:  return CARD_RESULT_BROKEN;
    case DC_SAVE_ENOMEM:    return CARD_RESULT_IOERROR;
    case DC_SAVE_EDISABLED: return CARD_RESULT_NOCARD;
    default:                return CARD_RESULT_IOERROR;
    }
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo_DC* fileInfo) {
    CARDOpenSlot* slot;
    char vname[16];
    unsigned int len = 0;
    int rc;

    if (!dc_card_vmu_name(fileName, vname)) return CARD_RESULT_NAMETOOLONG;

    fileInfo->chan = chan;
    fileInfo->offset = 0;

    slot = dc_card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;
    strcpy(slot->filename, vname);

    /* 0x72000 is what the game asks for; a shorter payload on the VMU is read
     * into the front of the staging buffer and the rest stays zero. */
    slot->staging_size = 0x72000;
    slot->staging = dc_card_staging_alloc(slot->staging_size);
    if (!slot->staging) { dc_card_slot_free(slot); return CARD_RESULT_IOERROR; }

    rc = dc_save_load((int)chan, vname, slot->staging, slot->staging_size, &len);
    if (rc != DC_SAVE_OK) {
        dc_card_slot_free(slot);
        return dc_card_map_err(rc);
    }

    fileInfo->length = (s32)slot->staging_size;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo_DC* fileInfo) {
    CARDOpenSlot* slot = dc_card_slot_find(fileInfo);
    s32 result = CARD_RESULT_READY;
    if (slot) {
        if (slot->dirty && slot->staging) {
            int rc = dc_save_store((int)slot->owner->chan, slot->filename,
                                   slot->staging, slot->staging_size);
            if (rc != DC_SAVE_OK) {
                DC_LOGE("[DC/CARD] save to %s FAILED: %s — NOT persisted\n",
                        slot->filename, dc_save_strerror(rc));
                result = dc_card_map_err(rc);
            }
        }
        dc_card_slot_free(slot);
    }
    return result;
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo_DC* fileInfo) {
    CARDOpenSlot* slot;
    char vname[16];

    if (!dc_card_vmu_name(fileName, vname)) return CARD_RESULT_NAMETOOLONG;
#ifndef DC_HOST_STUB
    if (!dc_vmu_dev((int)chan)) return CARD_RESULT_NOCARD;
#endif

    fileInfo->chan = chan;
    fileInfo->offset = 0;
    fileInfo->length = (s32)size;

    slot = dc_card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;

    strcpy(slot->filename, vname);
    slot->staging_size = size;
    slot->staging = dc_card_staging_alloc(size);
    if (!slot->staging) { dc_card_slot_free(slot); return CARD_RESULT_IOERROR; }
    slot->dirty = 1;
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size,
                    void* fileInfo, void* callback) {
    s32 result = CARDCreate(chan, fileName, size, (CARDFileInfo_DC*)fileInfo);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDRead(CARDFileInfo_DC* fileInfo, void* buf, s32 length, s32 offset) {
    CARDOpenSlot* slot = dc_card_slot_find(fileInfo);
    if (!slot || !slot->staging) return CARD_RESULT_NOFILE;
    if (offset < 0 || length < 0) return CARD_RESULT_IOERROR;
    if ((u32)offset + (u32)length > slot->staging_size) return CARD_RESULT_IOERROR;
    memcpy(buf, slot->staging + offset, (size_t)length);
    return CARD_RESULT_READY;
}

s32 CARDReadAsync(void* fileInfo, void* buf, s32 length, s32 offset, void* callback) {
    s32 result = CARDRead((CARDFileInfo_DC*)fileInfo, buf, length, offset);
    if (callback) ((void (*)(s32, s32))callback)(0, result);
    return result;
}

s32 CARDWrite(CARDFileInfo_DC* fileInfo, const void* buf, s32 length, s32 offset) {
    CARDOpenSlot* slot = dc_card_slot_find(fileInfo);
    if (!slot || !slot->staging) return CARD_RESULT_NOFILE;
    if (offset < 0 || length < 0) return CARD_RESULT_IOERROR;
    if ((u32)offset + (u32)length > slot->staging_size) return CARD_RESULT_IOERROR;
    memcpy(slot->staging + offset, buf, (size_t)length);
    slot->dirty = 1;
    /* NOTE (§3.2): m_card.c issues DCFlushRange on its save buffers. That is a
     * REAL cache op on SH-4 and happens on the caller's side; nothing extra is
     * needed here because the staging buffer is only read by the CPU. */
    return CARD_RESULT_READY;
}

s32 CARDWriteAsync(void* fileInfo, const void* buf, s32 length,
                   s32 offset, void* callback) {
    s32 result = CARDWrite((CARDFileInfo_DC*)fileInfo, buf, length, offset);
    if (callback) ((void (*)(s32, s32))callback)(0, result);
    return result;
}

s32 CARDDelete(s32 chan, const char* fileName) {
    char vname[16];
    if (!dc_card_vmu_name(fileName, vname)) return CARD_RESULT_NAMETOOLONG;
    return dc_card_map_err(dc_save_erase((int)chan, vname));
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, void* callback) {
    s32 result = CARDDelete(chan, fileName);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDGetResultCode(s32 chan) { (void)chan; return CARD_RESULT_READY; }

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    /* Reports the VMU's real free space, not the GameCube card's. The game
     * uses this to decide whether a save can be created — lying here would
     * turn "not enough space" into a corrupt half-written save
     * (kb/platform-api-save-card.md, CARDProbeEx row). */
    int blocks = dc_save_free_blocks((int)chan);
    if (blocks < 0) {
        if (byteNotUsed) *byteNotUsed = 0;
        if (filesNotUsed) *filesNotUsed = 0;
        return dc_card_map_err(blocks);
    }
    if (byteNotUsed) *byteNotUsed = blocks * VMU_BLOCK_BYTES;
    if (filesNotUsed) *filesNotUsed = blocks;
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    (void)chan;
    if (size) *size = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    /* The honest capacity, in the units the game expects. See CARDFreeBlocks. */
#ifndef DC_HOST_STUB
    if (!dc_vmu_dev((int)chan)) {
        if (memSize) *memSize = 0;
        if (sectorSize) *sectorSize = CARD_SECTOR_SIZE;
        return CARD_RESULT_NOCARD;
    }
#else
    (void)chan;
#endif
    if (memSize) *memSize = VMU_USER_BYTES;
    if (sectorSize) *sectorSize = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbe(s32 chan) {
#ifndef DC_HOST_STUB
    return dc_vmu_dev((int)chan) ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
#else
    (void)chan; return CARD_RESULT_NOCARD;
#endif
}

s32 CARDCheck(s32 chan) {
#ifndef DC_HOST_STUB
    return dc_vmu_dev((int)chan) ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
#else
    (void)chan; return CARD_RESULT_NOCARD;
#endif
}

s32 CARDCheckAsync(s32 chan, void* callback) {
    s32 result = CARDCheck(chan);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

typedef struct {
    char fileName[32];
    u32  length;
    u32  time;
    u8   gameName[4];
    u8   company[2];
    u8   bannerFormat;
    u32  iconAddr;
    u16  iconFormat;
    u16  iconSpeed;
    u32  commentAddr;
    u32  offsetBanner;
    u32  offsetBannerTlut;
    u32  offsetIcon[8];
    u32  offsetIconTlut;
    u32  offsetData;
} CARDStat;

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo;
    memset(stat, 0, sizeof(CARDStat));
    return CARD_RESULT_READY;
}
s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo; (void)stat;
    return CARD_RESULT_READY;
}
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, void* stat, void* callback) {
    (void)fileNo; (void)stat;
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

/* Read the old name, write it under the new one, then delete the old — in that
 * order, so a failure anywhere leaves the original intact. VMU flash has no
 * atomic rename and there is no room for a second copy of a save. */
s32 CARDRename(s32 chan, const char* oldName, const char* newName) {
#if defined(DC_HOST_STUB) || defined(DC_CARD_READONLY) || defined(DC_CARD_DISABLE)
    (void)chan; (void)oldName; (void)newName;
    return CARD_RESULT_NOPERM;
#else
    char vold[16], vnew[16];
    u8* buf;
    unsigned int len = 0;
    int rc;
    if (!dc_card_vmu_name(oldName, vold) || !dc_card_vmu_name(newName, vnew))
        return CARD_RESULT_NAMETOOLONG;
    if (!strcmp(vold, vnew)) return CARD_RESULT_READY;

    buf = (u8*)malloc(0x72000);
    if (!buf) return CARD_RESULT_IOERROR;
    rc = dc_save_load((int)chan, vold, buf, 0x72000, &len);
    if (rc != DC_SAVE_OK) { free(buf); return dc_card_map_err(rc); }
    rc = dc_save_store((int)chan, vnew, buf, len);
    free(buf);
    if (rc != DC_SAVE_OK) return dc_card_map_err(rc);
    dc_save_erase((int)chan, vold);
    return CARD_RESULT_READY;
#endif
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, void* callback) {
    s32 result = CARDRename(chan, oldName, newName);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

/* CARDFormat would reformat the player's whole VMU, wiping every other game's
 * save. The GameCube prompt that leads here says "format memory card", which
 * on a Dreamcast means something far more destructive than the player agreed
 * to. Refuse, loudly, and let the game report a card it cannot use. */
s32 CARDFormat(s32 chan) {
    (void)chan;
    DC_LOGE("[DC/CARD] CARDFormat refused: that would erase every game's save "
            "on this VMU, not just ours. Use the Dreamcast BIOS file manager.\n");
    return CARD_RESULT_NOPERM;
}
s32 CARDFormatAsync(s32 chan, void* callback) {
    s32 result = CARDFormat(chan);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

/* Called from pc_m_card.c to find an existing save. On DC there is no
 * directory of .gci files, so this answers for the VMU.
 *
 * ⚠ It returns 0 even when a save IS present, and that is deliberate, not a
 * bug: pc_m_card.c takes the path this returns and fopen()s it. Until the
 * stdio bridge exists (file header), handing back a real-looking path makes
 * the game print "found" and then fail to open it, which is strictly worse
 * than "no save". The VMU state is logged so the log still tells the truth. */
int pc_card_scan_for_gci(s32 chan, char* out_path, int out_size) {
    dc_card_selftest();
    (void)out_path; (void)out_size;
#ifndef DC_HOST_STUB
    if (!dc_vmu_dev((int)chan)) {
        DC_LOGE("[DC/CARD] scan chan %d: no VMU\n", (int)chan);
    } else if (dc_save_present((int)chan, DC_SAVE_VMU_FILE)) {
        DC_LOGE("[DC/CARD] scan chan %d: '%s' IS on the VMU, but pc_m_card.c "
                "loads through stdio and the VFS bridge is not built yet — "
                "reporting no save (kb/save-budget.md §7.8)\n",
                (int)chan, DC_SAVE_VMU_FILE);
    } else {
        DC_LOGE("[DC/CARD] scan chan %d: VMU present, no '%s' on it\n",
                (int)chan, DC_SAVE_VMU_FILE);
    }
#else
    (void)chan;
#endif
    return 0;
}

#endif /* !DC_CARD_STANDALONE */
