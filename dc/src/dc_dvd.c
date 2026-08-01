/* dc_dvd.c - DVD filesystem over the KOS VFS (/cd).
 *
 * Replaces pc/src/pc_dvd.c. The PC build parsed a GameCube ISO at runtime; the
 * DC build reads an OFFLINE-EXTRACTED layout burned onto the CD (PLAN §5), so
 * pc_disc.c moves to tools/ and this file talks to /cd.
 *
 * THREE CONTRACT POINTS (design doc §3.5) — breaking any of them re-opens
 * every DVD caller in the game:
 *   1. DVDReadAsyncPrio performs the whole read INLINE and calls the callback
 *      BEFORE returning TRUE. Every "async" DVD path in the game therefore
 *      completes before the caller's next statement. Validated across a full
 *      playthrough on the base port. A read-ahead thread must stay a PREFETCH
 *      layer under a still-synchronous DVDRead, never a real async API.
 *   2. DVDConvertPathToEntrynum INTERNS unknown paths and returns the index;
 *      it never returns -1 for an unknown file the way real GameCube does.
 *      Failure is deferred to DVDFastOpen.
 *   3. DVDFastOpen writes into the game's 0x3C-byte DVDFileInfo at hand-picked
 *      offsets: handle/sentinel at +0x18, startAddr at +0x30, length at +0x34.
 *      Those come from dolphin/dvd.h and must not move.
 *
 * CD-R STREAMS AT ~500 KB/s. The base port's known 8.7 s stall was on a much
 * faster medium. The read-ahead ring (mem-budget bucket 10, 3 x 128 KB) is
 * NOT implemented in this pass and is the first thing M2 needs.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"

/* ==========================================================================
 * Disc ID
 * ========================================================================== */
typedef struct {
    char gameName[4];
    char company[2];
    u8   diskNumber;
    u8   gameVersion;
    u8   streaming;
    u8   streamBufSize;
    u8   padding[22];
} DVDDiskID;

/* GAFE01, USA Rev 0 — the only supported source disc. gameVersion drives the
 * zurumode flags in boot.c, so it must stay 0. */
static DVDDiskID disk_id = {
    { 'G', 'A', 'F', 'E' }, { '0', '1' }, 0, 0, 0, 0, { 0 }
};

DVDDiskID* DVDGetCurrentDiskID(void) { return &disk_id; }

/* ==========================================================================
 * Path interning
 * ==========================================================================
 * mem-budget §4.1 shrinks the PC table (512 x 260 B = 133,120) to ~16 KB. A
 * shared string pool does that without truncating long archive paths.
 */
#define DC_DVD_MAX_ENTRIES 512
#define DC_DVD_PATH_POOL   12288

static u16  dvd_path_off[DC_DVD_MAX_ENTRIES];
static char dvd_path_pool[DC_DVD_PATH_POOL];
static int  dvd_entry_count = 0;
static int  dvd_pool_used = 0;

static const char* dvd_entry_path(int idx) {
    if (idx < 0 || idx >= dvd_entry_count) return NULL;
    return dvd_path_pool + dvd_path_off[idx];
}

s32 DVDConvertPathToEntrynum(const char* path) {
    int i, len;

    if (!path) return -1;
    for (i = 0; i < dvd_entry_count; i++)
        if (strcmp(dvd_path_pool + dvd_path_off[i], path) == 0)
            return i;

    len = (int)strlen(path) + 1;
    if (dvd_entry_count >= DC_DVD_MAX_ENTRIES ||
        dvd_pool_used + len > DC_DVD_PATH_POOL) {
        DC_LOGE("[DC/DVD] entry table full (%d entries, %d/%d pool bytes); "
                "cannot register %s\n",
                dvd_entry_count, dvd_pool_used, DC_DVD_PATH_POOL, path);
        return -1;
    }

    dvd_path_off[dvd_entry_count] = (u16)dvd_pool_used;
    memcpy(dvd_path_pool + dvd_pool_used, path, (size_t)len);
    dvd_pool_used += len;
    return dvd_entry_count++;
}

/* ==========================================================================
 * DVDFileInfo accessors — offsets are part of the contract, do not move.
 * ========================================================================== */
#define DVD_FI_HANDLE 0x18
#define DVD_FI_START  0x30
#define DVD_FI_LENGTH 0x34

static u32* dvd_fi_handle(void* fi) { return (u32*)((u8*)fi + DVD_FI_HANDLE); }
static u32* dvd_fi_start(void* fi)  { return (u32*)((u8*)fi + DVD_FI_START); }
static u32* dvd_fi_length(void* fi) { return (u32*)((u8*)fi + DVD_FI_LENGTH); }

/* ==========================================================================
 * KOS VFS glue
 * ==========================================================================
 * VERIFY (M0): kos/fs.h — fs_open(path, O_RDONLY), fs_close, fs_read,
 * fs_seek(fd, off, SEEK_SET), fs_total(fd). These are the KOS VFS primitives
 * rather than newlib stdio so that the read-ahead layer can sit underneath
 * without fighting FILE* buffering.
 */
#define DC_CD_PREFIX "/cd"

static void dc_dvd_make_path(char* out, size_t outsz, const char* path) {
    if (!path) { out[0] = '\0'; return; }
    if (path[0] == '/')
        snprintf(out, outsz, DC_CD_PREFIX "%s", path);
    else
        snprintf(out, outsz, DC_CD_PREFIX "/%s", path);
}

BOOL DVDFastOpen(s32 entrynum, void* fileInfo) {
    const char* path = dvd_entry_path(entrynum);
    char full[320];

    if (!path) return FALSE;
    dc_dvd_make_path(full, sizeof(full), path);

#ifdef DC_HOST_STUB
    (void)full;
    return FALSE;
#else
    {
        file_t fd = fs_open(full, O_RDONLY);
        if (fd == FILEHND_INVALID) {
            DC_LOG("[DC/DVD] miss: %s\n", full);
            return FALSE;
        }
        memset(fileInfo, 0, 0x3C);
        *dvd_fi_handle(fileInfo) = (u32)fd;
        *dvd_fi_start(fileInfo)  = 0;
        *dvd_fi_length(fileInfo) = (u32)fs_total(fd);
        return TRUE;
    }
#endif
}

BOOL DVDOpen(const char* filename, void* fileInfo) {
    s32 entry = DVDConvertPathToEntrynum(filename);
    if (entry < 0) return FALSE;
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(void* fileInfo) {
    u32 h = *dvd_fi_handle(fileInfo);
#ifndef DC_HOST_STUB
    if (h && h != (u32)FILEHND_INVALID) fs_close((file_t)h);
#endif
    (void)h;
    *dvd_fi_handle(fileInfo) = 0;
    return TRUE;
}

BOOL DVDFastClose(void* fileInfo) { return DVDClose(fileInfo); }

static s32 dc_dvd_read_impl(void* fileInfo, void* buf, s32 length, s32 offset) {
    u32 h = *dvd_fi_handle(fileInfo);
    if (!h) return -1;
#ifdef DC_HOST_STUB
    (void)buf; (void)length; (void)offset;
    return -1;
#else
    if (h == (u32)FILEHND_INVALID) return -1;
    if (fs_seek((file_t)h, offset, SEEK_SET) < 0) return -1;
    return (s32)fs_read((file_t)h, buf, (size_t)length);
#endif
}

s32 DVDReadPrio(void* fileInfo, void* buf, s32 length, s32 offset, s32 prio) {
    u64 t0 = dc_time_us();
    s32 r;
    (void)prio;

    /* THE READ-AHEAD LAYER GOES HERE (mem-budget bucket 10, PLAN §5):
     * a 3 x 128 KB ring filled by a low-priority KOS thread, checked before
     * falling through to the blocking read. It must remain invisible to the
     * caller — DVDRead stays synchronous (contract point 1). */
    r = dc_dvd_read_impl(fileInfo, buf, length, offset);

    {
        u64 dt = dc_time_us() - t0;
        if (dt > 50000ull)      /* the base port's stall-hunting threshold */
            DC_LOGE("[DC/DVD] slow read: %d bytes in %u.%03u ms\n",
                    (int)length, (unsigned)(dt / 1000), (unsigned)(dt % 1000));
    }
    return r;
}

s32 DVDRead(void* fileInfo, void* buf, s32 length, s32 offset) {
    return DVDReadPrio(fileInfo, buf, length, offset, 2);
}

u32 DVDGetLength(void* fileInfo) { return *dvd_fi_length(fileInfo); }

typedef void (*dc_DVDCallback)(s32, void*);

/* CONTRACT POINT 1: the callback fires BEFORE this returns. */
BOOL DVDReadAsyncPrio(void* fileInfo, void* buf, s32 length, s32 offset,
                      dc_DVDCallback callback, s32 prio) {
    s32 nread = DVDReadPrio(fileInfo, buf, length, offset, prio);
    if (callback) callback(nread, fileInfo);
    return TRUE;
}

void OSDVDFatalError(void) {
    DC_LOGE("[DC/DVD] fatal disc error\n");
}

void DVDInit(void) {
    dc_mem_note(DCMEM_DISC_IO,
                (ptrdiff_t)(sizeof(dvd_path_off) + sizeof(dvd_path_pool)));
    DC_LOGE("[DC/DVD] VFS root " DC_CD_PREFIX ", entry table %u B "
            "(NO read-ahead yet; CD-R is ~500 KB/s)\n",
            (unsigned)(sizeof(dvd_path_off) + sizeof(dvd_path_pool)));
}

void DVDSetAutoFatalMessaging(BOOL enable) { (void)enable; }
s32  DVDGetFileInfoStatus(void* fileInfo) { (void)fileInfo; return 0; }
s32  DVDGetTransferredSize(void* fileInfo) { (void)fileInfo; return 0; }
s32  DVDGetDriveStatus(void) { return 0; }
s32  DVDCancel(void* block) { (void)block; return 0; }
BOOL DVDCancelAsync(void* block, void* callback) {
    (void)block; (void)callback; return TRUE;
}
s32  DVDChangeDisk(void* block, void* id) { (void)block; (void)id; return 0; }
BOOL DVDChangeDiskAsync(void* block, void* id, void* callback) {
    (void)block; (void)id; (void)callback; return TRUE;
}
/* Always DVD_STATE_END, for the same reason DVDReadAsyncPrio is synchronous. */
s32  DVDGetCommandBlockStatus(void* block) { (void)block; return 0; }
BOOL DVDPrepareStreamAsync(void* fi, u32 len, u32 off, void* cb) {
    (void)fi; (void)len; (void)off; (void)cb; return TRUE;
}
s32  DVDCancelStream(void* block) { (void)block; return 0; }

/* ==========================================================================
 * pc_disc shims
 * ==========================================================================
 * pc/src/pc_assets.c is REFERENCED by the DC build unchanged (it is 30k lines
 * of generated dispatch table and its data source is the only thing that
 * changes). It needs exactly three symbols from pc_disc.c, which is host-side
 * now. PLAN §5 has the offline extractor put main.dol and the decompressed
 * foresta.rel on the disc, so these read them from /cd.
 *
 * WARNING (mem-budget C6): loading BOTH images at once is a multi-megabyte
 * transient — the kb measures a 15.64 MB REL transient on the base port. That
 * transient is the single largest RAM problem after the arena and it is why
 * the asset residency pool exists. This code path WILL NOT FIT in 16 MB and is
 * here so the tree links and so M1 can measure the real sizes.
 */
static u8* dc_read_whole_file(const char* full, unsigned int* out_size) {
#ifdef DC_HOST_STUB
    (void)full; (void)out_size;
    return NULL;
#else
    file_t fd = fs_open(full, O_RDONLY);
    size_t sz;
    u8* buf;
    if (fd == FILEHND_INVALID) return NULL;
    sz = (size_t)fs_total(fd);
    buf = (u8*)malloc(sz);
    if (!buf) { fs_close(fd); return NULL; }
    if (fs_read(fd, buf, sz) != (ssize_t)sz) { free(buf); fs_close(fd); return NULL; }
    fs_close(fd);
    dc_mem_note(DCMEM_ASSET_POOL, (ptrdiff_t)sz);
    if (out_size) *out_size = (unsigned int)sz;
    return buf;
#endif
}

int pc_disc_is_open(void) { return 1; }

u8* pc_disc_extract_dol(void) {
    unsigned int sz = 0;
    u8* p = dc_read_whole_file(DC_CD_PREFIX "/main.dol", &sz);
    if (!p) DC_LOGE("[DC/DVD] " DC_CD_PREFIX "/main.dol MISSING — assets will "
                    "not load (run the tools/ extractor)\n");
    else    DC_LOGE("[DC/DVD] main.dol %u B resident\n", sz);
    return p;
}

u8* pc_disc_extract_rel(void) {
    unsigned int sz = 0;
    /* Already Yaz0-decompressed by the offline extractor — do NOT decompress
     * here, the transient would be twice as large. */
    u8* p = dc_read_whole_file(DC_CD_PREFIX "/foresta.rel", &sz);
    if (!p) DC_LOGE("[DC/DVD] " DC_CD_PREFIX "/foresta.rel MISSING — assets "
                    "will not load (run the tools/ extractor)\n");
    else    DC_LOGE("[DC/DVD] foresta.rel %u B resident\n", sz);
    return p;
}
