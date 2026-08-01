/* dc_card.c - GameCube memory card API on a Dreamcast VMU.
 *
 * Replaces pc/src/pc_card.c (one GCI file per card slot on disk). The 29 CARD*
 * entry points and the 20-byte CARDFileInfo layout are unchanged — the game
 * allocates those structs — only the backend moves.
 *
 * THE PROBLEM (PLAN §6, the honest second-hardest after RAM):
 *   GCI_FILE_DATA_SIZE = mCD_LAND_SAVE_SIZE = 0x72000 = 466,944 B (~456 KB).
 *   VMU user space  ~= 100 KB (200 x 512 B blocks).
 *   That is a 4.7x overshoot. The plan, in order:
 *     1. measure real entropy and LZ-compress (M1, host-side, on real saves)
 *     2. segment across 2 VMUs (the DC controller has two slots)
 *     3. trim the save (DC-edition cut, breaks GCI interchange, last resort)
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
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"

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
    char filename[64];
} CARDOpenSlot;

static CARDOpenSlot card_slots[CARD_MAX_OPEN];
static int card_mounted[2] = { 0, 0 };

/* VMU port mapping. KOS names maple units "a1".."d2"; the DC controller in
 * port A has two VMU slots, which is the hardware behind PLAN §6 option 2
 * (segment a save across two VMUs). Channel 0 -> a1, channel 1 -> a2. */
static const char* card_vmu_path[2] = { "/vmu/a1", "/vmu/a2" };

static const char* dc_card_dir(s32 chan) {
    return (chan >= 0 && chan <= 1) ? card_vmu_path[chan] : card_vmu_path[0];
}

static int dc_card_name_safe(const char* name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
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
    if (size > 100u * 1024u) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            DC_LOGE("[DC/CARD] WARNING: %u B card image staged in RAM. VMU user "
                    "space is ~100 KB (200 x 512 B). PLAN 6 compression must "
                    "land before this can be saved at all.\n", (unsigned)size);
        }
    }
    return p;
}

static void dc_card_full_path(char* out, size_t outsz, s32 chan, const char* name) {
    snprintf(out, outsz, "%s/%s", dc_card_dir(chan), name);
}

/* ==========================================================================
 * VMU I/O
 * ==========================================================================
 * VERIFY (M0): KOS exposes VMUs through the VFS at /vmu/<port><unit>, and
 * whole-file writes go through vmu_pkg_build() so the file gets a proper VMS
 * header (icon, description, CRC) and shows up in the BIOS file manager.
 * Reading is a plain fs_read; writing a raw fs_write produces a file the BIOS
 * will not display. Both halves are stubbed rather than guessed.
 */
static int dc_vmu_read_file(const char* full, u8* dst, u32 size, u32* out_len) {
#ifdef DC_HOST_STUB
    (void)full; (void)dst; (void)size; (void)out_len;
    return 0;
#else
    file_t fd = fs_open(full, O_RDONLY);
    ssize_t n;
    if (fd == FILEHND_INVALID) return 0;
    n = fs_read(fd, dst, (size_t)size);
    fs_close(fd);
    if (n < 0) return 0;
    if (out_len) *out_len = (u32)n;
    return 1;
#endif
}

static int dc_vmu_write_file(const char* full, const u8* src, u32 size) {
    (void)full; (void)src; (void)size;
    /* Real version: build a vmu_pkg_t (app id "OPENCROSSING", icon, eyecatch,
     * description), vmu_pkg_build() it, then fs_write the packaged blob. */
    DC_UNIMPLEMENTED_NOTE("VMU write via vmu_pkg_build + fs_write; needs PLAN 6 "
                          "compression first (466,944 B does not fit 100 KB)");
    return 0;
}

/* ==========================================================================
 * CARD API
 * ========================================================================== */
void CARDInit(void) {
    memset(card_slots, 0, sizeof(card_slots));
    DC_LOGE("[DC/CARD] slot A -> %s, slot B -> %s\n",
            card_vmu_path[0], card_vmu_path[1]);
}

s32 CARDMount(s32 chan, void* workArea, void* detachCallback) {
    (void)workArea; (void)detachCallback;
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
#ifndef DC_HOST_STUB
    /* VERIFY: maple_enum_type(chan, MAPLE_FUNC_MEMCARD) is the presence test;
     * a missing VMU must report NOCARD so the game shows its own "no memory
     * card" flow instead of failing later inside a save. */
    if (!maple_enum_type(chan, MAPLE_FUNC_MEMCARD)) {
        DC_LOGE("[DC/CARD] no VMU in slot %d\n", (int)chan);
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

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo_DC* fileInfo) {
    char full[128];
    CARDOpenSlot* slot;
    u32 len = 0;

    if (!dc_card_name_safe(fileName)) return CARD_RESULT_NAMETOOLONG;
    dc_card_full_path(full, sizeof(full), chan, fileName);

    fileInfo->chan = chan;
    fileInfo->offset = 0;

    slot = dc_card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;

    strncpy(slot->filename, fileName, sizeof(slot->filename) - 1);
    slot->filename[sizeof(slot->filename) - 1] = '\0';

    /* 0x72000 is what the game asks for; anything smaller on the VMU is read
     * into the front of the staging buffer and the rest stays zero. */
    slot->staging_size = 0x72000;
    slot->staging = dc_card_staging_alloc(slot->staging_size);
    if (!slot->staging) { dc_card_slot_free(slot); return CARD_RESULT_IOERROR; }

    if (!dc_vmu_read_file(full, slot->staging, slot->staging_size, &len)) {
        dc_card_slot_free(slot);
        return CARD_RESULT_NOFILE;
    }

    fileInfo->length = (s32)slot->staging_size;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo_DC* fileInfo) {
    CARDOpenSlot* slot = dc_card_slot_find(fileInfo);
    if (slot) {
        if (slot->dirty && slot->staging) {
            char full[128];
            dc_card_full_path(full, sizeof(full), slot->owner->chan, slot->filename);
            if (!dc_vmu_write_file(full, slot->staging, slot->staging_size))
                DC_LOGE("[DC/CARD] save to %s FAILED (not persisted)\n", full);
        }
        dc_card_slot_free(slot);
    }
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo_DC* fileInfo) {
    CARDOpenSlot* slot;

    if (!dc_card_name_safe(fileName)) return CARD_RESULT_NAMETOOLONG;

    fileInfo->chan = chan;
    fileInfo->offset = 0;
    fileInfo->length = (s32)size;

    slot = dc_card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;

    strncpy(slot->filename, fileName, sizeof(slot->filename) - 1);
    slot->filename[sizeof(slot->filename) - 1] = '\0';
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
    char full[128];
    if (!dc_card_name_safe(fileName)) return CARD_RESULT_NAMETOOLONG;
    dc_card_full_path(full, sizeof(full), chan, fileName);
#ifndef DC_HOST_STUB
    fs_unlink(full);   /* VERIFY: KOS kos/fs.h fs_unlink */
#endif
    return CARD_RESULT_READY;
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, void* callback) {
    s32 result = CARDDelete(chan, fileName);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDGetResultCode(s32 chan) { (void)chan; return CARD_RESULT_READY; }

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    (void)chan;
    /* Reports the VMU's real user space, not the GameCube card's. The game
     * uses this to decide whether a save can be created — lying here would
     * turn "not enough space" into a corrupt half-written save. */
    if (byteNotUsed) *byteNotUsed = 200 * 512;   /* 200 usable blocks */
    if (filesNotUsed) *filesNotUsed = 200;
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    (void)chan;
    if (size) *size = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    (void)chan;
    if (memSize) *memSize = 128 * 1024;   /* VMU flash */
    if (sectorSize) *sectorSize = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbe(s32 chan) { (void)chan; return CARD_RESULT_READY; }
s32 CARDCheck(s32 chan) { (void)chan; return CARD_RESULT_READY; }
s32 CARDCheckAsync(s32 chan, void* callback) {
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
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

s32 CARDRename(s32 chan, const char* oldName, const char* newName) {
    (void)chan; (void)oldName; (void)newName;
    DC_UNIMPLEMENTED_NOTE("VMU rename: read, delete, rewrite under the new name");
    return CARD_RESULT_READY;
}
s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, void* callback) {
    s32 result = CARDRename(chan, oldName, newName);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDFormat(s32 chan) { (void)chan; return CARD_RESULT_READY; }
s32 CARDFormatAsync(s32 chan, void* callback) {
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

/* Called from pc_m_card.c (referenced unchanged by the DC build) to find an
 * existing save. On DC there is no directory of .gci files — the VMU holds one
 * named file — so this reports the canonical path if the VMU has it. */
int pc_card_scan_for_gci(s32 chan, char* out_path, int out_size) {
    char full[128];
    dc_card_full_path(full, sizeof(full), chan, "ANIMAL_CROSSING");
#ifndef DC_HOST_STUB
    {
        file_t fd = fs_open(full, O_RDONLY);
        if (fd == FILEHND_INVALID) return 0;
        fs_close(fd);
    }
#else
    return 0;
#endif
    snprintf(out_path, (size_t)out_size, "%s", full);
    return 1;
}
