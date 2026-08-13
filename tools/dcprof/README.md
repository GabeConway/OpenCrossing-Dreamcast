# P2 — gprof off a burned CD-R, and the same profile out of Flycast

**The question this exists to answer:** the game is materially slower on a real
Dreamcast than in Flycast, and nobody has ever measured *where* the extra time
goes. Flycast models **no instruction cache, no operand cache and no disc seek
time**, so it is structurally incapable of answering it (`kb/RESUME.md` §0h,
CLAUDE.md §1 rule 12). P1 (`dc/src/dc_pmcr.c`) says *what* — icache stalls,
operand fills, issue rate. P2 says ***which function***.

⭐ **P2 WORKS. VALIDATED END TO END 2026-08-12** in Flycast, console sink, town
build: the link, the arming, the allocation, the sampler, the encoder, the
decoder and `sh-elf-gprof` all did their jobs. §4 carries the numbers. **The
hardware half has not been run** — that is the whole remaining risk, and §8 is
the list.

🔴 **IT IS TWO IMAGES, NOT ONE.** Earlier revisions of this file promised the
sink was chosen at runtime so one CDI would serve both targets. **That is
falsified** — the SD probe does not fail cleanly, it wedges (§1). Build
`DC_GPROF_SD=0` for Flycast and `DC_GPROF_SD=1` for hardware. §7 explains why
that costs the diff nothing.

| piece | file |
|---|---|
| the guest-side sinks + triggers | `dc/src/dc_profdump.c`, `dc/include/dc_profdump.h` |
| the frame hook | `dc/src/dc_vi.c`, `DC_GPROF_TICK()` in the presented path |
| the build knobs + `GPROF_LDFLAGS` | `dc/Makefile`, the `DC_GPROF` block |
| forwarding into the container | `dc/build-dc.sh`, `ENVARGS` |
| the host-side decoder (**console sink only**) | `tools/dcprof/decode_gmon.py` |
| the profiler itself | KOS `addons/libgprof` (`gmon.c`, `arch/dreamcast/trap.c`) — unmodified |
| the plan, the constraints, the hardware procedure | `kb/hardware-profiling.md` |

---

## 1. The ideas, because none of them is obvious

### `-pg` goes on the LINK line and on NO translation unit

KOS's libgprof has two independent halves. `gprof_mcount()` builds a call graph
and needs every function compiled `-pg` (on SH-4 that is a `trapa #33` at every
function entry, plus `-fno-omit-frame-pointer`, usually `-fno-inline`) — which
would change every instruction timing in the image and make the measurement
describe a build nobody ships.

`histogram_callback()` is the other half and it is a **plain PC sampler**: it
rides `thd_poll`, reads `thd_current`'s saved PC on every reschedule, and **does
not involve `mcount` at all**.

So: **no TU is compiled `-pg`; `-pg` appears only on the link line.** Every
optimized object stays byte-identical to the shipping build. `kos-cc` (which
`kos-c++` execs) scans its arguments for `-pg` and auto-links
`-Wl,--whole-archive -lgprof -Wl,--no-whole-archive`, placed **after** our
objects and before `KOS_LIBS`. We lose the call graph and do not want it: a flat
profile is what a hardware-vs-emulator diff reads.

⭐ **Verified on the real 3,936-object link, 2026-08-12** — not just read out of
the wrapper script. It links, exactly one `gprof_init` survives, the image boots
and arms.

⭐ **`-pg` is keyed into `$(LINKSTATE)`, NOT `$(FLAGSTATE)`.** It changes no
object by construction, so keying it into the compile stamp would rebuild
~3,900 TUs to relink one ELF. Flipping `DC_GPROF` still relinks, because a kill
switch that silently does nothing is worse than no kill switch (CLAUDE.md §1).
`make optreport` prints the `P2 gprof link` line if you need to see the state.

### `gprof_init()` must be overridden, and that is not optional

KOS calls `gprof_init()` from `arch_main()` **before constructors and before
`main()`**. libgprof's own `gprof_init()` would call `monstartup()` over the
whole `.text` at that instant, and `monstartup()` immediately does
`fopen(path, "w")`. On a burned CD-R the default path `/pc/gmon.out` **does not
exist** — `/pc` is the dc-load host filesystem — so the open fails, the context
latches `GMON_PROF_ERROR`, and the run produces nothing.

`dc_profdump.c` therefore defines a **strong `gprof_init()` of its own** that
mounts the sink first and only then calls `monstartup()`. Our objects precede
`-lgprof` on the link line and the link already carries
`-Wl,--allow-multiple-definition`, so ours is the definition placed and
libgprof's is discarded.

⚠️ It runs **before `main()`**, which means a hang inside it is a *silent boot
failure* — the log stops after KOS's own `vid_set_mode` line with nothing to say
which of setenv / nmmgr / `thd_set_hz` / `monstartup` swallowed it. That is what
the first `DC_GPROF=1` image did. It now emits **six stage markers**,
`[GPROF] g0` through `[GPROF] g5 monstartup returned` — if a boot dies, the last
`g` you see names the stage.

### And the console sink stores nothing

`/prof` is a KOS VFS with a real `write` and nothing else. Every byte handed to
it is zero-run-RLE'd, base64'd, and pushed straight out of `dbgio` as framed
console lines. **No buffer, no file, no RAM** — which matters, because the
profile buffers are already 1.5 MB (§5).

⭐ **The RLE is what makes this affordable, and it beat its own estimate by two
orders of magnitude.** A histogram is almost all zeros. **Measured: 612,433 raw
bytes → 1,585 encoded → 28 console lines.** The "~836 KB of base64 = ~145 s of
serial" figure this file and `dc_profdump.c`'s header comment both carried is
**obsolete** — it assumed base64 with no RLE in front of it.

⚠️ Encoded size scales with **non-empty bins**, not with `.text`, so a longer
run encodes larger. Still nowhere near the naive number.

### 🔴 The SD sink is at DUMP TIME, and it is NOT switched by `GMON_OUT_PREFIX`

`DC_GPROF_SD=1` compiles in a second sink: a real FAT32 file on a serial-port SD
adapter. **The card is not brought up in `gprof_init()`.** Three reasons, each
decisive on its own:

1. **`monstartup()` runs before `main()` and allocates ~1.5 MB.** Deferring the
   *allocation* to mid-run would put it against a fragmented heap in a build
   whose whole difficulty is RAM. The allocation stays at boot.
2. **The SCIF reader reconfigures SCIF for bit-banged SPI and collides with
   KOS's SCIF console.** SD-at-boot means muting the console at boot — and
   muting at `main()` is the documented way to stop this port booting at all
   (`kb/traps.md`; it cost the 2026-08-08 `-f` burn). At dump time the
   measurement is over, so the collision is free.
3. 🔴 **`gmon.c` MUST NEVER `fopen()` THE CARD.** It opens gmon.out with mode
   `"a"`, and newlib turns `"a"` into `O_WRONLY|O_CREAT|O_APPEND`. `O_APPEND` is
   **`0x08`**; KOS's `O_MODE_MASK` is **`0x0f`**. So `O_APPEND` lands *inside*
   the mode nibble, and `fs_fat_write()` does

   ```c
   mode = fh[fd].mode & O_MODE_MASK;
   if(mode != O_WRONLY && mode != O_RDWR) { errno = EBADF; return -1; }
   ```

   `0x9` is neither — **every append write to a FAT32 card fails with EBADF.**
   ⚠️ And `fs_fat_open()` does *not* check the mode, so **the open succeeds**;
   only the writes fail, reported into a console this code has already disabled.
   (`"w"` is fine: `0x601 & 0x0f == 1 == O_WRONLY`, which is exactly why the trap
   is invisible until the burn.)

**So delete the `GMON_OUT_PREFIX` mechanism from your model of this.** Earlier
revisions described the sink being swapped mid-run by re-`setenv`-ing it between
the header write and the histogram write. It does not work that way. `gmon.c`
writes to `/prof` for the **whole run**, and `prof_write()` decides where the
bytes land — console-encoded, or `fwrite()`n into a card file **this code opened
itself in `"w"` and holds open**. One writer, one mode, no append.

Consequence: the 20-byte gmon header has already gone down the **console** by
the time the card comes up, so `dc_prof_sd_open()` writes those 20 bytes to the
card by hand — cookie `"gmon"`, `int32` version 1 little-endian, twelve zero
bytes, checked against `gmon.c`'s `gmon_hdr_t`. Without them the file is not a
gmon.out.

### 🔴 And the probe can HANG — which is why it is two images

Earlier revisions claimed *"a `DC_GPROF_SD=1` image whose card does not
enumerate falls back to the console sink, so one CDI is correct either way."*
**Falsified 2026-08-12.**

`sci_spi_rw_byte()` (`kernel/arch/dreamcast/hardware/sci.c`) waits on RDRF in a
`do { status = SCSSR1; if(status & ORER) {...} } while(!(status & RDRF));` loop
with **no cycle cap**. Every TDRE wait in that same file is capped by
`SCI_MAX_WAIT_CYCLES`; **all four RDRF waits in the SPI helpers are not.** The
SH-4's SCSSR1 reset value is **0x84** (TDRE|TEND set, RDRF and ORER clear) —
which passes every bounded TDRE gate, lets `sci_init()` return `SCI_OK`, then
pins `sd_init_ex()`'s first `spi_rw_byte(0xFF)` **forever**. Nothing wired to
SCI ever sets RDRF or ORER.

SCIF is bounded but useless as a fallback: `acmd41_loop()` runs up to
`MAX_RETRIES` **5000** iterations, each two `sd_send_cmd()`s with a **500 ms**
`thd_poll` ceiling.

**MEASURED:** a `DC_GPROF_SD=1` image in Flycast printed the pre-mute marker at
frame 300 and never returned — **on both `DC_GPROF_SD_IF` settings**.

⭐ **The pre-mute marker is why we know that.** `dc_profdump_flush_now()` emits
`[GPROF] dump: muting console, probing SD` *before* `dbgio_flush();
dbgio_disable();`. Without it, a wedge inside the muted probe window is
indistinguishable from the game hanging — that ambiguity cost four Flycast runs
and a misdiagnosis (the 1000 Hz sampler was blamed) before anyone could
attribute it. **Never remove that line.**

Full mechanism, the MBR + 4 KB-cluster card requirements and the mandatory
`fs_fat_sync` / `fs_fat_unmount`: `kb/hardware-profiling.md` §6.

---

## 2. Build

`DC_GPROF=1` is the only switch a caller needs; the `-pg` on the link comes
with it. Everything is off by default: each knob is guarded in `dc/Makefile`
with `ifneq ($(DC_GPROF…),)`, so **empty means off** and a build without
`DC_GPROF` is byte-identical to a shipping image. `DC_GPROF=0` is also an
explicit off — `GPROF_LDFLAGS` uses `$(filter-out 0,…)` so a literal `0` does
not put `-pg` on the line.

🔴 **BEFORE 2026-08-12 THIS SECTION PUBLISHED A BUILD LINE THAT DID NOTHING.**
`dc/Makefile` had no `DC_GPROF` block and `dc/build-dc.sh` forwarded none of the
knobs, so every variable was dropped on the floor and the image contained no
profiler. Both now exist. **Discard any P2 result dated earlier.**

### ⭐ The Flycast half — this is the line the §4 result came from

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
DC_GPROF=1 DC_GPROF_SD=0 DC_GPROF_HZ=0 DC_GPROF_DUMP_FRAME=300 \
DC_SECTION_ORDER=0 bash dc/build-dc.sh
```

### The hardware half — same line, three changes

```bash
… DC_GPROF=1 DC_GPROF_SD=1 DC_GPROF_HZ=0 DC_GPROF_DUMP_FRAME=2400 \
DC_SECTION_ORDER=0 DC_CDI_PAD=1 bash dc/build-dc.sh
```

The frame cap goes up because a human takes longer to reach the town than an
emulator does; the chord (§3) is the real trigger and the cap is the backstop.
`DC_CDI_PAD=1` is for the burn — Flycast wants the unpadded `mkdcdisc -N`.

**Why each of the non-obvious flags is there:**

- 🔴 **`DC_SECTION_ORDER=0` — F5 MUST BE OFF.** `dc/section-order.txt` was
  generated from a **keeplist-full** link. Applied to a **keeplist-town** link —
  which is what a profiling build is (§5) — the image **hangs in Flycast inside
  `maple_wait_scan()`**, i.e. in `arch_auto_init()`, before `main()` and before
  `gprof_init()` can print `g0`. `DC_SECTION_ORDER=0` on the identical tree
  boots clean. **Proven by a four-build matrix, 2026-08-12.** ⚠️ Hardware boots
  the F5-on keeplist-town image fine, so this is a Flycast-only failure — but
  **F5 is clearly not inert when the link it describes changes**, and both halves
  must match anyway.
- ⚠️ **`DC_ARAM_WINDOW` stays at its shipping 1048576.** Lowering it, as this
  file once instructed, corrupts the measurement — §5.
- **`keeplist-town`, not `keeplist-full`.** That is the reclaim that makes
  `monstartup()` fit, and it is measured to (§5).
- **`DC_GPROF_HZ=0`** — KOS's 100 Hz. §6.

⚠️ **Not in the validated line, but available:** `DC_AUTOSTART=300
DC_AUTOWALK=900` make a Flycast run unattended and repeatable — without them the
guest sits on the title screen (`BUILDING-DC.md`). Add them if you want a
hands-off A/B; they were not part of the run in §4.

**All nine `DC_GPROF*` knobs are forwarded by `dc/build-dc.sh`** (`ENVARGS`).
⚠️ Forwarding is not optional and its absence is silent — a knob the script does
not forward is simply off, which is how `DC_EMU64_HIST` sat unrun for two
sessions (`kb/RESUME.md` §10).

| knob | default | what it does |
|---|---|---|
| `DC_GPROF=1` | off | compiles `dc_profdump.c`, arms the sampler, puts `-pg` on the link |
| `DC_GPROF_SD=1` | off | compiles in the SD sink and adds `-lkosfat`. 🔴 **Hardware only** — the probe wedges when there is no card |
| `DC_GPROF_SD_IF=0\|1\|2` | **0** | `0` = SCIF only (jj1odm-style; **always fails cleanly**). `1` = SCI only (SWAT-style) ⚠️ **can hang forever**, set only if you know the adapter is SCI. `2` = SCIF then SCI ⚠️ **do not use** — it inherits the hang as its fallback path |
| `DC_GPROF_DUMP_FRAME=N` | 1800 | dump at N **presented** frames. Use **300** in Flycast, **2400** on hardware. `0` = never, leaving the chord or `DC_GPROF_FLUSH()` |
| `DC_GPROF_CHORD=0` | on | disable the **L + R + START** manual dump trigger, §3 |
| `DC_GPROF_HZ=N` | **0 (KOS's 100)** | scheduler rate = sample rate. ⭐ **Leave it at 0** — see §6 |
| `DC_GPROF_SPAN=N` | 0 (all `.text`) | profile only N bytes from the low address — the RAM escape hatch. **Not needed with keeplist-town**, §5 |
| `DC_GPROF_LOW=0x…` | `__executable_start` | move the window's base |
| `DC_GPROF_COLS=N` | 76 | base64 chars per console line |

`DC_GPROF_PREFIX` is a **string** and has no `dc/Makefile` knob of its own; pass
it through `DC_XDEFS` (`-DDC_GPROF_PREFIX=\"…\"`). 🔴 **It is NOT the seam for
the SD card and never was — `DC_GPROF_SD` is.** A non-`/prof` value skips
mounting the `/prof` VFS and sends `fopen` at whatever real filesystem owns the
path; nothing mounts that filesystem for you, so on a CD-R boot the open fails
and the run produces nothing. Leave it alone.

**Never combine with:**

- `DC_CONSOLE_MUTE` — it calls `dbgio_disable()`; the console dump goes
  nowhere. ⭐ **The one exception is `DC_GPROF_SD=1`**, where the dump never
  touches `dbgio` and the mute is not only safe but wanted (`kb/RESUME.md` §6:
  on a measuring burn the SCIF log measures itself — and §4 now shows
  `scif_write` at 0.50 % *inside its own profile*).
- `DC_SCIF_FAST=1` — **on hardware a coder's cable will not sync at 1.5 Mbps**.
  The capture is garbage and you will not know until you decode it.
- `DC_PMCR_HUD=1` — P1's HUD writes a screenful of uncached VRAM per presented
  frame and lands inside the profile as renderer time.

---

## 3. Run and capture

### Flycast (console sink)

```bash
harness/dc/console.sh dc/build/OpenCrossing.cdi --timeout 180 \
  > /tmp/flycast-run.json 2> /tmp/flycast-console.log
```

The timeout must exceed `DC_GPROF_DUMP_FRAME` frames **plus** the dump itself —
which is now small: 28 lines of ~91 characters is ~2.5 KB, under a second even
at 57,600 baud (arithmetic, not measured).

Watch for, in order:

```
[GPROF] g0 … g5 monstartup returned                 <- boot staging; a missing g names the hang
[GPROF] arming: range 8c010000..8c266062, predicted alloc 1506528 B, hz=100, dump at frame 300
[GPROF] Total memory allocated: 1506528 bytes       <- gmon.c's own line; the alloc SUCCEEDED
[GPROF] BEGIN v=1 enc=z0b64 cols=76 low=8c010000 high=8c266062 alloc=1506528
[GPROF] <index> <base64> <cksum>                    <- 28 of these
[GPROF] END lines=28 raw=612433 enc=1585 crc32=2fd07100
```

If `Total memory allocated` never appears, `posix_memalign` failed — the buffers
did not fit. Go to §5.

### Real hardware, SD sink (`DC_GPROF_SD=1`)

The adapter and the coder's cable are the **same physical port**, so there is no
console at all. Prepare the card as **MBR-partitioned FAT32 with 4 KB
clusters**:

- a raw superfloppy (filesystem at sector 0, no partition table) does not
  enumerate — `sd_blockdev_for_partition()` reads an MBR;
- `fat_fs_init_ex()` allocates **eight cluster-sized cache buffers**, so a
  32 KB-cluster card asks for **~260 KB** mid-run, in a build whose entire
  difficulty is RAM.

Then:

1. Adapter in the serial port, card in the adapter.
2. Boot the burned CDI, play into the town, walk to somewhere worth profiling,
   stand still.
3. ⭐ **Hold L + R + START.** Both analog triggers past 200 plus Start, read
   straight off maple in `dc_prof_chord_down()` so it works whether or not the
   game is polling. `DC_GPROF_CHORD=0` turns it off.
4. **Expect a multi-second hitch** — that is `_mcleanup()` pushing ~600 KB
   through bit-banged SPI.
5. Power off. Take the card to the host.

⚠️ The chord also reaches the game as Z/L/R+Start and opens the menu. Accepted:
the dump ends the useful part of the run anyway.

| file on the card | what it is |
|---|---|
| `/sd/gmon.<pid>` | the gmon.out. Goes **straight** to `sh-elf-gprof`, §4 |
| `/sd/gprof.txt` | the receipt: path, **size**, short writes, interface, range, predicted alloc, hz, frames presented, and the `sh-elf-gprof` command |

⭐ **Read `gprof.txt` FIRST.** With no console it is the only thing that can say
"it armed", it says outright that **20 bytes means the histogram never landed**
(the exact symptom the `O_APPEND` trap of §1 produces), and its `frames` line is
the denominator for a chord-ended run, whose duration nothing else recorded.

### Real hardware, console sink

Possible — burn the `DC_GPROF_SD=0` image, attach a coder's cable, capture SCIF
at KOS's default **57,600 baud, 8N1, no flow control**, wait for `[GPROF] END`.
⚠️ But it puts `scif_write` in the profile and it is not what the SD sink exists
for. Use it only if the adapter is not available.

---

## 4. ⭐ Decode and report — with the numbers from the validated run

🔴 **`decode_gmon.py` IS FOR THE CONSOLE SINK ONLY.** It parses base64 payload
lines out of a `console.log`; hand it the raw `gmon.<pid>` off the SD card and it
dies with *"no `[GPROF] BEGIN` line"*. **An SD file is already a gmon.out — skip
this step entirely and go to `sh-elf-gprof`.**

Console captures only:

```bash
python3 tools/dcprof/decode_gmon.py flycast-console.log -o fc.gmon.out
```

The decoder verifies, in order: contiguous line indices, a per-line checksum,
the encoded byte count, the raw byte count, a CRC-32 over the raw bytes, and
the `gmon` cookie. **Any failure is fatal and nothing is written** — a gmon.out
that is 90 % of a histogram is not 90 % of a profile, it is a wrong profile
that still opens in gprof and still prints plausible percentages. Each failure
names the console line that caused it. It also prints the histogram summary and
warns below 500 samples.

**What it printed on 2026-08-12:** a **612,433-byte** gmon.out — **306,190
bins, 31,010 samples in 470 non-empty bins, profrate 100 Hz**.

Then, inside the SDK container (`sh-elf-gprof` is `$KOS_GPROF`):

```bash
docker run --rm --platform linux/arm64 \
  -v "$(pwd)":/work \
  opencrossing-dc:sdk bash -c \
  'cd /work && sh-elf-gprof -b -p dc/build/AnimalCrossing.elf fc.gmon.out > fc.gprof.txt'
```

Identical for an SD run — just point it at the copied `gmon.<pid>` instead. That
is the whole reason the SD sink is worth having: no encoder, no decoder, no
console, nothing between the histogram and the report that can lose bytes.

`-p` is the flat profile; `-b` drops the explanatory blurb. **Do not ask for
`-q`** — there is no call graph, by design.

**The top of the validated flat profile:**

```
94.76%  293.86s  thd_idle_task
 0.66%    2.05s  vid_waitvbl
 0.51%    1.59s  dc_gx_backend_submit
 0.50%    1.56s  scif_write
 0.38%    1.17s  RspStart
 0.20%    0.63s  emu64::set_position(unsigned int)
 0.18%    0.55s  cull_batch(emu64*)
 0.17%    0.53s  GXPosition3f32
```

### 🔴 IDLE DOMINATES AT 94.76 % — COMPARE NON-IDLE SHARES, NEVER ABSOLUTE SECONDS

Every non-idle entry above is a share of the ~5 % that is left, so a "0.51 %"
line is really ~10 % of the work the machine actually did. **Renormalise over
non-idle samples before comparing anything**, and never quote the seconds
column — it is samples ÷ a *nominal* profrate (§6).

⭐ **Hardware, being slower, should idle less and give better signal than this.**
That is an argument for the burn, not against it.

Note `scif_write` at 1.56 s: **the console tax showing up in its own profile.**
That is the argument for the SD sink.

⚠️ **`dc/build/AnimalCrossing.elf` must be the exact ELF the CDI was built
from.** The CDI's `1ST_READ.BIN` is scrambled and stripped; a rebuilt ELF
silently shifts every address and gprof will confidently name the wrong
functions. The build writes an `<image>.src.json` sidecar with the ELF's sha256
(`harness/dc/README.md`) — check it before reporting a result. ⭐ With two images
(§7) this matters *more*, not less: symbolise each half against **its own** ELF.

---

## 5. The memory problem — solved by the keep list, and now measured

`monstartup()` allocates, for a range of `T` bytes:

| buffer | size | used by |
|---|---|---|
| histogram | `T/8 × 2` = `0.250 T` | the sampler — **the only one we use** |
| froms | `T/16 × 2` = `0.125 T` | `gprof_mcount` |
| nodes | `T×2/100 × 12` = `0.240 T` | `gprof_mcount` |
| | **≈ `0.615 T`** | |

⚠️ **59 % of that allocation is dead weight.** `froms` and `nodes` only ever get
written by `gprof_mcount`, and nothing is compiled `-pg`, so they stay zero for
the whole run and `write_arcs()` emits nothing. Removing them needs a change in
KOS's libgprof, not here; until then a profiling build pays for them.

**⭐ MEASURED on the 2026-08-12 keeplist-town link — IT FITS:**

```
range 8c010000..8c266062
predicted alloc                  1,506,528 B     (dc_prof_predict_alloc)
[GPROF] Total memory allocated:  1506528 bytes   (gmon.c's own line)
```

The prediction and the reality agree to the byte, and the game went on to
render, sample and dump. Earlier revisions of this file computed
**1,541,792 B against ~499,088 B of headroom, short by 1,042,704 B** — correct
arithmetic *about a keeplist-full build*, and not the situation a profiling
build is in.

A profiling build is a **measurement** build, not the shipping one, so it may
reclaim RAM the shipping build spends:

| reclaim | frees | why it is safe here | why it is not free |
|---|---|---|---|
| ⭐ `DC_STUB_KEEP=…/keeplist-town.txt` | **~899,640** | `keeplist-full`'s extra `src/data/model/` families are content, not code — nothing about the frame's *timing* is falsified | fewer models render, so it is not the shipping frame |
| `DC_GPROF_SPAN=N` | `0.615 × (T − N)` | costs no game state at all | ⚠️ changes the denominator — see below. **Not needed** |

🔴 **`DC_ARAM_WINDOW=131072` IS NOT ON THIS LIST, AND EARLIER REVISIONS OF THIS
FILE PUT IT IN THE BUILD LINE.** It frees ~917,504 B, which is why it looked like
the obvious lever. Then `kb/RESUME.md` §5 measured it: matched 420 s runs at
131072 vs 1048576 gave disc reads **106 → 4,183**, bytes off disc
**4.3 MB → 137.9 MB**, evictions **68 → 4,173**. **Disc timing is precisely one
of the things hardware has and Flycast does not**, so shrinking the window pours
~40× the disc traffic into the run being profiled — and `dc_dvd_read_yielding()`
is already the most over-represented symbol in this profile (§6). It would not
merely perturb the measurement, it would **manufacture its answer**. Keep it at
1048576.

### If you ever do need to narrow the range

`DC_GPROF_SPAN=N` profiles only the first `N` bytes of `.text`, scaling the
allocation by `N/T`. Choose `N` from `tools/dcopt/icache_map.py`'s hot-symbol
listing: take the highest address you care about, subtract `0x8c010000`, round
up. ⚠️ **Note that F5 is OFF in a profiling build (§2)**, so the low end of
`.text` is *not* the packed draw loop any more — a narrow window catches
whatever the linker's natural order put there, which is a much weaker
justification than the one earlier revisions of this file gave.

⚠️ **A narrowed range changes the DENOMINATOR, it does not merely lose
resolution.** `histogram_callback` **drops** a sample whose PC is out of range —
it is not counted anywhere. So gprof's percentages become "percent of samples
that landed in the window", not "percent of the frame". Say so when quoting a
narrowed run, and only ever compare narrowed-vs-narrowed with the same `N`.

---

## 6. Caveats you must state when quoting a number

**1. Samples accrue per RESCHEDULE, not per timer tick — while the gmon header
claims `profrate = thd_get_hz()`.** `histogram_callback` runs from `thd_poll`,
so a sample is taken on every reschedule: the `THD_SCHED_HZ` preemption tick
*plus* every **voluntary yield**. That biases the profile toward code that
blocks and makes the seconds column a fiction built on a nominal rate:

- `dc_dvd_read_yielding()` (`dc/src/dc_dvd.c`) yields before the seek and
  between chunks — **the single most over-represented function in this
  profile**, and worse the more the disc is read.
- `dc_audio_disc_yield()`, and anything that sleeps on a mutex.

⭐ **This bias mostly cancels in the Flycast-vs-hardware diff and does NOT
cancel in an absolute profile. Treat the output as RELATIVE SHARES.**

**2. ⭐ The sampler is NOT expensive — this was suspected and CLEARED.**
`histogram_callback` runs once per reschedule and is **~20 instructions**: two
range compares, a divide-by-8, a `uint16` increment. `thd_poll` parks the caller
in `STATE_POLLING` via `thd_block_now` — **no busy-wait, no starvation.**
`[PERF]` read **24–28 FPS** with the sampler armed. The stalls that were blamed
on it were the SD probe wedge (§1).

**3. `DC_GPROF_HZ=0` (KOS's 100 Hz) is the recommended value, and the validated
one.** It gave **31,010 samples in 470 bins from a 300-frame run** — not a
shortage. ⚠️ **Do NOT repeat the claim that 1000 Hz is slow**: that theory was
raised to explain the early hangs and then **refuted**. `DC_GPROF_HZ=1000` is
still available (KOS's ceiling) if a real run ever comes back sample-starved; it
costs 10× the scheduler entries, which is real frame time and appears in the
profile as scheduler work, so **never quote FPS from a `DC_GPROF_HZ=1000` run.**

**4. The dump is a stall, but a small one on the console sink.** 612,433 bytes
of histogram became **1,585 encoded bytes in 28 lines** — ~2.5 KB of console,
under a second at 57,600 baud. ⚠️ The **SD** sink's cost is **unmeasured**:
bit-banged SPI writing ~600 KB unencoded, budget a multi-second hitch. Either
way it does not corrupt the measurement — sampling has already stopped inside
`_mcleanup()` before the write, so the stall lands *after* the profile, not in
it. It does poison the `[PERF]`/`[PMCR]` window it falls in; take FPS from
before the dump.

**5. Profiling is on from before `main()`.** Boot, asset loading and the title
screen are all in the histogram. Either subtract them by eye or drive into the
town early so they are a small fraction.

**6. No console device, no profile — on the console sink.**
`dbgio_dev_select_auto()` picks dcload if present, else SCIF. With no cable and
no host, the dump is written to nothing and the run looks identical to a
successful one. ⭐ **This is the failure `DC_GPROF_SD=1` exists to remove**: the
card takes the histogram and `/sd/gprof.txt` is the receipt that proves it ran.

**7. A chord-ended run has a length nobody wrote down.** The frame cap makes
every unattended run the same duration; a human pressing L+R+START does not.
`/sd/gprof.txt`'s `frames` line is the only record of it. Quote it, and do not
diff a chord run against a frame-capped one without accounting for it.

**8. The town reseeds every boot** (`kb/RESUME.md` §8), so two runs are two
different towns. Compare profile **shapes**, not single-symbol percentages.

---

## 7. The A/B recipe — two images, and why that is fine

1. Build **both** halves from the §2 lines: `DC_GPROF_SD=0` for Flycast,
   `DC_GPROF_SD=1 DC_CDI_PAD=1` for the burn. Keep **each** build's
   `dc/build/AnimalCrossing.elf` and its `.src.json` — each report is symbolised
   against its own.
2. Flycast: `harness/dc/console.sh … --timeout 180 2> flycast-console.log`,
   then `decode_gmon.py`.
3. Hardware: burn, card in, drive to the town, **L + R + START**, power off,
   copy `/sd/gmon.<pid>` and read `/sd/gprof.txt`.
4. `sh-elf-gprof -b -p` each against its own ELF.
5. **Renormalise both over non-idle samples**, then diff by symbol.
6. Read it against P1: run `DC_PMCR=1` separately (**burn only — Flycast
   reports zero for every event**) and check that the functions that grew in
   the hardware profile are the ones the `istall` counter says are stalling.

⭐ **Two images do NOT weaken the diff.** Three reasons:

1. `gprof` symbolises against **each run's own ELF**, so the halves need not be
   the same binary — only the same *code*.
2. `-pg` is **link-line only**, so the profiled game code is byte-identical
   between them by construction (§1).
3. `dc_profdump.c` and `libkosfat` are **not on the hot path** — they run once,
   at the dump, after `_mcleanup()` has stopped the sampler.

It was never optional anyway: Flycast has no SD adapter, and on hardware the
adapter occupies the serial port, so there is no console. The one real asymmetry
is `scif_write` in the console half (§4) — subtract it, or read the SD half as
the cleaner one.

⚠️ **Do not use the chord in an A/B without accounting for the frame counts** —
the emulator side cannot press it, so the runs differ in length. `/sd/gprof.txt`
carries the hardware side's `frames`.

**What a result looks like.** If the icache hypothesis is right, the hardware
profile's extra non-idle share concentrates in the widely-scattered call sites
of the draw loop — `dc_gx_backend_submit` is already #3 in the Flycast profile
and `tools/dcopt/icache_map.py` puts it at **10,120 B, 1.24× the whole 8 KB
icache** — and `dc/section-order.txt`, regenerated for the right link, should
move it. If the extra time is instead in `dc_dvd_read_yielding` and its callers,
the answer is disc seek, which Flycast sets to zero (`FastGDRomLoad=yes`), and
the fix is read-ahead, not layout.

⚠️ **A flat profile is a ranking, not a mechanism.** It says which function
grew. It does not say why, and with no call graph it cannot distinguish "this
function got slower" from "this function was called more times". Pair every
claim with a counter.

---

## 8. Status — 2026-08-12

**⭐ VALIDATED, end to end, Flycast + console sink + town build:**

| piece | evidence |
|---|---|
| the full `DC_GPROF=1` link, 3,936 objects, one surviving `gprof_init` | it booted and armed |
| `monstartup()` fits — 1,506,528 B alongside keeplist-town | `[GPROF] Total memory allocated: 1506528 bytes` |
| the sampler samples | 31,010 samples in 470 non-empty bins, 306,190 bins, 100 Hz |
| the z0/base64 console sink | 612,433 raw → 1,585 enc → 28 lines |
| `decode_gmon.py` round-trip | indices, per-line checksums, counts, CRC-32 and cookie all clean |
| `sh-elf-gprof -b -p` symbolisation | §4's flat profile |
| the frame-cap trigger | dumped at frame 300 |
| the sampler's cost | `[PERF]` 24–28 FPS with it armed |

**Still unproven — this is the remaining risk:**

- 🔴 **No hardware run.** The Flycast half is done; the console half of the diff
  is the deliverable and it does not exist yet.
- 🔴 **No card has ever been mounted by this code.** The MBR requirement, the
  4 KB cluster requirement, the 20-byte header hand-off, the
  `fs_fat_sync`/`fs_fat_unmount` requirement and the SCIF probe are all read out
  of KOS source. The only observed fact about the SD path is that **the SCI
  probe wedges** (§1).
- 🔴 **The pad chord has never been pressed.** Whether `maple_dev_status()`
  returns a usable `cont_state_t` from inside the vblank path on hardware is
  unobserved.
- ⚠️ **The SD adapter's throughput on either interface is unmeasured.** Run the
  stock `sd-speedtest` example before assuming anything.
- ⚠️ **Whether the sampler perturbs frame pacing on hardware**, where it
  competes with the audio thread for reschedules, is unmeasured. Flycast said
  24–28 FPS.
