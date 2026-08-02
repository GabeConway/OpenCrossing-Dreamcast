# `dc/src/dc_mem_ledger.c` — runtime RAM ledger design (was mem-budget §5)

Design for making the RAM budget a runtime object that fails loudly: per-bucket
extents from `dc_mem_budget.h`, `_Static_assert` on over-commit, machine-parseable
`MEMLEDGER` lines for `harness/dc/smoke.sh`, `--wrap=malloc` capture, and the
32 MB-RAM-mod refusal. Read before touching `dc/include/dc_mem_budget.h` or the
ledger implementation. Bucket names refer to `kb/mem-budget-void-ledger.md` §4,
whose *numbers* are void even though the bucket taxonomy is what the header
encodes.

## 5. `dc/src/dc_mem_ledger.c` — design (task 5; **do not implement here**,
`dc/src/` is another agent's)

Purpose: make the table in §4 a runtime object, so the budget is *enforced*
rather than hoped, and an overrun is a loud, greppable failure rather than a
mysterious crash three scenes later.

### Files

- `dc/include/dc_mem_budget.h` — the §4 table as `#define DC_BUDGET_<BUCKET>`
  constants, plus `DC_RAM_BUDGET_BYTES (16*1024*1024)`. Generated from this kb
  file by a small host script so doc and code cannot drift.
- `dc/include/dc_mem_ledger.h` — API.
- `dc/src/dc_mem_ledger.c` — implementation.

### API sketch

```c
typedef enum {
    DCMEM_KOS, DCMEM_IMAGE_TEXT, DCMEM_IMAGE_DATA, DCMEM_IMAGE_BSS,
    DCMEM_JKRHEAP, DCMEM_ASSET_POOL, DCMEM_ARAM_GRAPH, DCMEM_AUDIO,
    DCMEM_DISC_IO, DCMEM_PVR_STAGING, DCMEM_STACKS, DCMEM_NBUCKET
} dc_mem_bucket_t;

void  dc_mem_ledger_init(void);                     /* before any big alloc */
void* dc_mem_alloc(dc_mem_bucket_t, size_t, size_t align, const char* owner);
void  dc_mem_free (dc_mem_bucket_t, void*);
void  dc_mem_note (dc_mem_bucket_t, ptrdiff_t);     /* allocators we don't own */
void  dc_mem_frame(void);                           /* per-frame poll, O(1) */
void  dc_mem_report(int verbose);
```

### Boot-time behaviour

1. **Measure the image, don't trust a header.** Use the linker symbols
   (`_executable_start`, `_etext`, `__data_start`, `_edata`, `__bss_start`,
   `end`) to fill `DCMEM_IMAGE_*` exactly. This catches "someone added a
   200 KB array to BSS" on the boot after it happens, not at M5.
2. **Refuse the RAM mod.** Read `_arch_mem_top`. If it is `0x8E000000`
   (32 MB), print `MEMLEDGER WARN 32MB-detected budget-still-16MB` and clamp
   the arena to 16 MB unless built with `DC_ALLOW_32MB`. This is the
   mechanical guard that stops the stock-16 MB line eroding by accident
   (PLAN §3.1 / CLAUDE.md hardware contract).
3. **Carve one contiguous arena** from `&end` to the clamped top, then split
   it into fixed, non-overlapping extents per bucket from
   `dc_mem_budget.h`. A bucket physically cannot overrun into another — an
   overrun is a `NULL` from that bucket's sub-allocator, attributed by name.
4. `_Static_assert(sum(DC_BUDGET_*) <= DC_RAM_BUDGET_BYTES)` — over-commit is
   a **compile error**, which is the cheapest possible place to catch it.
5. Print the ledger, one machine-parseable line per bucket:
   `MEMLEDGER <bucket> budget=<B> reserved=<B> used=<B> peak=<B>` and a final
   `MEMLEDGER TOTAL used=<B> budget=16777216 margin=<B>`. `harness/dc/smoke.sh`
   greps these; the M2 gate ("RAM ledger ≤ 16 MB true") becomes a grep instead
   of a judgement call.

### Runtime behaviour

- Per-bucket `cur` / `peak` / `peak_tag` / `largest_free`, updated on every
  alloc/free (counters only — no free-list walking on the frame path).
- `dc_mem_tag(const char*)` from scene transitions (town / house / shop /
  museum / island / event) so peaks are attributable to a place.
- **Third-party allocations get captured**: link with
  `-Wl,--wrap=malloc,--wrap=free,--wrap=memalign` so KOS's and GLdc's
  allocations land in `DCMEM_KOS` / `DCMEM_PVR_STAGING` rather than
  disappearing. Without this the ledger lies.
- Overrun policy:
  - `DC_MEM_STRICT` (default for dev/Flycast builds): print
    `MEMLEDGER FAIL bucket=<n> owner=<s> want=<B> free=<B> ra=<addr>`,
    dump the whole table, then `arch_exit()`. Loud, immediate, greppable.
  - Release builds: log, return `NULL`, let the caller degrade — the asset
    pool evicts LRU and retries once; anything else treats `NULL` as fatal.
- Fragmentation watch: warn when a bucket's `largest_free < 25 %` of its free
  bytes. This is how we'll catch the `__osMalloc` fragmentation risk noted in
  §4.2 before it becomes a hang.
- `dc_mem_frame()` also feeds the VMU LCD / debug overlay (PLAN §7 already
  wants a VMU display) — total used as a bar is free instrumentation.

### Hooks other agents must add (not in this file)

`dc/src/dc_os.c` (arena carve + `OSGetArenaLo/Hi`), `dc/src/dc_main.c`
(`dc_mem_ledger_init` first, `dc_mem_report` after boot and on exit),
`dc/src/dc_aram.c` (graph window `dc_mem_note`), the asset pool, the
read-ahead thread, and the `JKRHeap` shim.

---
