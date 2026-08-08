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
#include "dc_aram_lru.h"

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

/* ==========================================================================
 * Provenance ring + the pager's own file handles  (PLAN §3.1, dc_aram_lru.h)
 * ==========================================================================
 * The graph-ARAM pager needs to answer one question: "the bytes this DMA is
 * about to push into ARAM — where on the disc did they come from?" It can,
 * because JKRAramStream::writeToAram() (JKRAramStream.cpp:92-103) reads into a
 * buffer and DMAs from that same buffer on the very next statement, with no
 * intervening disc traffic. Recording the last few DVDRead destinations is
 * therefore enough to recover an exact (file, offset) for every archive byte,
 * and the pager can then re-read a range instead of holding it resident.
 *
 * Four slots, not one, because DVDReadAsyncPrio's callback can issue another
 * read before the caller's DMA. Four is arbitrary but generous; a miss is not
 * a correctness bug, it only means the pager treats that write as anonymous
 * and has to store it, which it counts.
 */
#define DC_DVD_PROV_SLOTS 4

/* defined below with the rest of the KOS VFS glue */
static void dc_dvd_make_path(char* out, size_t outsz, const char* path);

typedef struct {
    const u8* buf;      /* destination the read landed in */
    u32       len;      /* bytes actually read */
    u32       off;      /* byte offset within the file */
    s32       entry;    /* DVDConvertPathToEntrynum index, -1 if unknown */
} dc_dvd_prov;

static dc_dvd_prov dvd_prov[DC_DVD_PROV_SLOTS];
static int         dvd_prov_next = 0;

/* handle -> entrynum. DVDFastOpen knows the entrynum, dc_dvd_read_impl only
 * sees the DVDFileInfo, and the 0x3C-byte struct's spare fields belong to the
 * game (contract point 3) — so keep the association on our side. */
#define DC_DVD_FDMAP_SLOTS 8
static u32 dvd_fdmap_handle[DC_DVD_FDMAP_SLOTS];
static s32 dvd_fdmap_entry[DC_DVD_FDMAP_SLOTS];
static int dvd_fdmap_next = 0;

static void dc_dvd_fdmap_set(u32 handle, s32 entry) {
    int i;
    for (i = 0; i < DC_DVD_FDMAP_SLOTS; i++)
        if (dvd_fdmap_handle[i] == handle) { dvd_fdmap_entry[i] = entry; return; }
    dvd_fdmap_handle[dvd_fdmap_next] = handle;
    dvd_fdmap_entry[dvd_fdmap_next] = entry;
    dvd_fdmap_next = (dvd_fdmap_next + 1) % DC_DVD_FDMAP_SLOTS;
}

static s32 dc_dvd_fdmap_get(u32 handle) {
    int i;
    for (i = 0; i < DC_DVD_FDMAP_SLOTS; i++)
        if (dvd_fdmap_handle[i] == handle) return dvd_fdmap_entry[i];
    return -1;
}

static void dc_dvd_fdmap_clear(u32 handle) {
    int i;
    for (i = 0; i < DC_DVD_FDMAP_SLOTS; i++)
        if (dvd_fdmap_handle[i] == handle) { dvd_fdmap_handle[i] = 0;
                                             dvd_fdmap_entry[i] = -1; }
}

int dc_dvd_provenance(const void* buf, unsigned int len,
                      int* out_entry, unsigned int* out_off) {
#if DC_ARAM_LRU
    const u8* p = (const u8*)buf;
    int k;
    if (!p || len == 0) return 0;
    /* MOST RECENT FIRST, and this is not a nicety.
     * JKRAramStream::writeToAram() reuses ONE transfer buffer for every 32 KB
     * chunk of an archive, so all four ring slots hold the same `buf` with
     * different file offsets. Scanning forwards returns whichever stale offset
     * happens to sit in slot 0 — MEASURED 2026-08-02: it mapped every chunk of
     * forest_1st/forest_2nd to the wrong place in the file, none of the
     * extents coalesced, the 32-slot extent table overflowed after 32
     * transfers, and the rest of forest_2nd was counted LOST. Walk backwards
     * from the newest entry. */
    for (k = 0; k < DC_DVD_PROV_SLOTS; k++) {
        int i = (dvd_prov_next - 1 - k + 2 * DC_DVD_PROV_SLOTS) % DC_DVD_PROV_SLOTS;
        const dc_dvd_prov* r = &dvd_prov[i];
        if (!r->buf || r->entry < 0 || r->len == 0) continue;
        if (p >= r->buf && (u32)(p - r->buf) + len <= r->len) {
            if (out_entry) *out_entry = (int)r->entry;
            if (out_off)   *out_off   = (unsigned int)(r->off + (u32)(p - r->buf));
            return 1;
        }
    }
#else
    (void)buf; (void)len; (void)out_entry; (void)out_off;
#endif
    return 0;
}

/* The pager's own handles. The game deletes its JKRDvdFile when the archive
 * unmounts, and the whole point of the pager is to still be able to fault a
 * range in afterwards, so it opens its own. Two slots covers the two archives
 * that are mounted simultaneously (forest_1st + forest_2nd); a third mount
 * evicts LRU-by-use and costs one fs_open. */
#define DC_DVD_PAGER_FDS 3
static s32  dvd_pager_entry[DC_DVD_PAGER_FDS];
static u32  dvd_pager_fd[DC_DVD_PAGER_FDS];
static u32  dvd_pager_use[DC_DVD_PAGER_FDS];
static u32  dvd_pager_clock = 0;
static int  dvd_pager_inited = 0;

static unsigned int dvd_pager_reads = 0;
static unsigned int dvd_pager_bytes = 0;
static unsigned int dvd_pager_opens = 0;
static unsigned int dvd_pager_usec = 0;

int dc_dvd_pager_read(int entry, unsigned int off, void* dst, unsigned int len) {
#if DC_ARAM_LRU && !defined(DC_HOST_STUB)
    const char* path;
    char full[320];
    int i, victim;
    file_t fd = FILEHND_INVALID;
    u64 t0;
    s32 got;

    if (entry < 0 || !dst || len == 0) return -1;
    path = dvd_entry_path(entry);
    if (!path) return -1;

    if (!dvd_pager_inited) {
        for (i = 0; i < DC_DVD_PAGER_FDS; i++) dvd_pager_entry[i] = -1;
        dvd_pager_inited = 1;
    }

    t0 = dc_time_us();

    for (i = 0; i < DC_DVD_PAGER_FDS; i++)
        if (dvd_pager_entry[i] == entry) { fd = (file_t)dvd_pager_fd[i]; break; }

    if (fd == FILEHND_INVALID) {
        dc_dvd_make_path(full, sizeof(full), path);
        fd = fs_open(full, O_RDONLY);
        if (fd == FILEHND_INVALID) {
            DC_LOGE("[DC/ARAM] pager cannot open %s\n", full);
            return -1;
        }
        victim = 0;
        for (i = 0; i < DC_DVD_PAGER_FDS; i++) {
            if (dvd_pager_entry[i] < 0) { victim = i; break; }
            if (dvd_pager_use[i] < dvd_pager_use[victim]) victim = i;
        }
        if (dvd_pager_entry[victim] >= 0) fs_close((file_t)dvd_pager_fd[victim]);
        dvd_pager_entry[victim] = entry;
        dvd_pager_fd[victim] = (u32)fd;
        i = victim;
        dvd_pager_opens++;
    }
    dvd_pager_use[i] = ++dvd_pager_clock;

    /* The ARAM pager's read. Same helper as every other blocking read in the
     * program — it yields before the seek and between chunks. */
    got = dc_dvd_read_yielding((unsigned int)fd, dst, len, 1, off);

    dvd_pager_reads++;
    if (got > 0) dvd_pager_bytes += (unsigned int)got;
    dvd_pager_usec += (unsigned int)(dc_time_us() - t0);
    return (int)got;
#else
    (void)entry; (void)off; (void)dst; (void)len;
    return -1;
#endif
}

void dc_dvd_pager_stats(unsigned int* out_reads, unsigned int* out_bytes,
                        unsigned int* out_opens, unsigned int* out_usec) {
    if (out_reads) *out_reads = dvd_pager_reads;
    if (out_bytes) *out_bytes = dvd_pager_bytes;
    if (out_opens) *out_opens = dvd_pager_opens;
    if (out_usec)  *out_usec  = dvd_pager_usec;
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
        dc_dvd_fdmap_set((u32)fd, entrynum);   /* provenance, PLAN §3.1 */
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
    dc_dvd_fdmap_clear(h);
#ifndef DC_HOST_STUB
    if (h && h != (u32)FILEHND_INVALID) fs_close((file_t)h);
#endif
    (void)h;
    *dvd_fi_handle(fileInfo) = 0;
    return TRUE;
}

BOOL DVDFastClose(void* fileInfo) { return DVDClose(fileInfo); }

/* ==========================================================================
 * THE ONE PLACE A DISC READ MAY BLOCK — dc_dvd_read_yielding()
 * ==========================================================================
 * SECOND HARDWARE ITERATION, 2026-08-08. The first chunked read went into
 * dc_dvd_pager_read() only, and the burn came back "same problem but less
 * laser thrash" and then "actually I think it's still there". The chunking
 * worked; it was in the WRONG FUNCTION for most of the bytes.
 *
 * dc_dvd_pager_read() is the ARAM pager. **The game's own asset loading does
 * not go through it** — DVDRead/DVDReadPrio land in dc_dvd_read_impl() below,
 * and the DC_ASSET_STUB keep-list loader has two more `fs_seek` + `fs_read`
 * pairs of its own in dc_main.c. Those are the reads that run during a scene
 * load, they are the largest in the program, and every one of them was still a
 * single blocking call with no audio produced anywhere inside it.
 *
 * So the chunking lives HERE now, once, and every site calls it. Two rules it
 * encodes, both learned from the first iteration:
 *
 *   1. **Yield BEFORE the seek, not only between chunks.** A seek is 100-200 ms
 *      of head movement that no chunk size can subdivide, so the only defence
 *      is to enter it with a full ring. The ring is otherwise only as full as
 *      the last logic tick left it — and during a burst of back-to-back reads
 *      that is not full at all.
 *   2. **A short first chunk.** The seek is paid on the first read regardless;
 *      making that read small gets us back to a yield as soon as the head
 *      arrives, instead of adding a further 32 ms of transfer to it.
 *
 * Kill switch DC_DVD_READ_CHUNK=0 restores a single fs_read at every site.
 */
s32 dc_dvd_read_yielding(unsigned int fd, void* dst, unsigned int len,
                         int seek_valid, unsigned int off) {
#ifdef DC_HOST_STUB
    (void)fd; (void)dst; (void)len; (void)seek_valid; (void)off;
    return -1;
#else
#ifndef DC_DVD_READ_CHUNK
#define DC_DVD_READ_CHUNK 16384
#endif
#ifndef DC_DVD_FIRST_CHUNK
/* ~4 ms of CD-R transfer. Small enough that the yield after it lands almost
 * immediately after the head arrives; large enough not to turn one read into
 * hundreds of calls. */
#define DC_DVD_FIRST_CHUNK 2048
#endif

#if (DC_DVD_READ_CHUNK) > 0
    u32 done = 0;
    u32 want = (u32)(DC_DVD_FIRST_CHUNK);

    /* Rule 1: top up before we block, so the seek is covered by a full ring. */
    dc_audio_disc_yield();

    if (seek_valid && fs_seek((file_t)fd, (off_t)off, SEEK_SET) < 0) return -1;

    while (done < len) {
        s32 n;
        if (want > len - done) want = len - done;
        n = (s32)fs_read((file_t)fd, (u8*)dst + done, (size_t)want);
        if (n <= 0) break;
        done += (u32)n;
        if ((u32)n < want) break;               /* short read: EOF */
        if (done < len) dc_audio_disc_yield();
        want = (u32)(DC_DVD_READ_CHUNK);        /* rule 2: only the FIRST is short */
    }
    return (s32)done;
#else
    if (seek_valid && fs_seek((file_t)fd, (off_t)off, SEEK_SET) < 0) return -1;
    return (s32)fs_read((file_t)fd, dst, (size_t)len);
#endif
#endif
}

static s32 dc_dvd_read_impl(void* fileInfo, void* buf, s32 length, s32 offset) {
    u32 h = *dvd_fi_handle(fileInfo);
    if (!h) return -1;
#ifdef DC_HOST_STUB
    (void)buf; (void)length; (void)offset;
    return -1;
#else
    if (h == (u32)FILEHND_INVALID) return -1;
    /* ⭐ THE GAME'S OWN ASSET READS COME THROUGH HERE, and until 2026-08-08
     * they were one blocking fs_read with no audio produced inside them. This
     * is the path a scene load takes; the ARAM pager is not. */
    return dc_dvd_read_yielding(h, buf, (unsigned int)length, 1,
                                (unsigned int)offset);
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

#if DC_ARAM_LRU
    /* Record where these bytes came from. The graph-ARAM pager reads this ring
     * on the very next statement of JKRAramStream::writeToAram() to learn the
     * (file, offset) behind an ARAM write, which is what lets it drop the
     * bytes instead of holding them resident. See dc_aram_lru.h. */
    if (r > 0 && buf) {
        dc_dvd_prov* slot = &dvd_prov[dvd_prov_next];
        slot->buf   = (const u8*)buf;
        slot->len   = (u32)r;
        slot->off   = (u32)offset;
        slot->entry = dc_dvd_fdmap_get(*dvd_fi_handle(fileInfo));
        dvd_prov_next = (dvd_prov_next + 1) % DC_DVD_PROV_SLOTS;
    }
#endif

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
