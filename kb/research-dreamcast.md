# Dreamcast homebrew platform research (mid-2026)

Compiled 2026-08-01 from web research. Facts that shaped PLAN.md.

## KallistiOS (KOS)

- Actively maintained; most devs track master (last tag v2.2.2, 2024-04-18).
  https://github.com/KallistiOS/KallistiOS — docs https://kos-docs.dreamcast.wiki/
- Toolchain via dc-chain (sh-elf GCC + newlib); "latest GCC" supported, full
  C17/C++20. sm64-dc shipped on GCC 14.1 + KOS/GLdc master. DreamSDK R4
  (2025-08) ships GCC 15.
- Threads: kernel threads, C11/pthreads/std::thread. GDB stub (`gdb_init()`).
  VFS, networking. kos-ports: SDL, GLdc, zlib…
- Serious ports often carry modified KOS forks (dca3-kos:
  https://gitlab.com/skmp/dca3-kos). Docker images exist (Nold360/docker-kallistios-sdk, kosaio).
- ARM7 sound driver ships precompiled; AICAOS rewrite landed as KOS PRs
  #950/#951 (2025-03).

## PowerVR CLX2 (the GPU contract)

- Tile-based deferred, 32×32 tiles; per-tile depth/blend on-chip; **hardware
  order-independent translucency**. Scene = Opaque / Punch-Through /
  Translucent polygon lists (+ modifier volumes).
  https://www.copetti.org/writings/consoles/dreamcast/
- **No shaders. No T&L. One texture unit.** Per-poly fixed function:
  decal/modulate, per-vertex base + **offset (specular) color**, table fog,
  hardware bump-map texture format (tangent-space DOT3 — doom64-dc uses it
  for 16 dynamic lights @60 fps).
- Texture formats: ARGB1555/RGB565/ARGB4444; **twiddled** (Morton, pow2);
  **VQ-compressed** (~8:1, decoded free in hardware); paletted 4/8-bit
  (global 1024-entry palette state); stride textures for non-pow2; max
  1024×1024. https://kos-docs.dreamcast.wiki/group__pvr__txr__fmts.html
- Tools: **pvrtex** (TapamN, in KOS utils; VQ+mip+dither) — vendored by dca3;
  texconv https://github.com/tvspelsfreak/texconv
- GLdc: partial OpenGL 1.2 on PVR; immediate mode + vertex arrays;
  GL_ARB_multitexture implemented as second blended pass into TR list; no
  FBOs/texture matrices. Canonical: https://gitlab.com/simulant/GLdc
  (GitHub mirror Kazade/GLdc). Fast enough for sm64-dc at full speed.
- Fastest ports (dca3) skip GLdc → direct KOS PVR API + store queues.

## SH-4 / RAM

- 200 MHz; 8 KB I$ / 16 KB D$ (**8 KB usable as scratchpad — dca3 flips CCR
  to use operand cache as RAM**). FPU SIMD: **FTRV** (4×4 matrix × vec4 vs
  XMTRX back bank), **FIPR** dot, FSRRA 1/√. T&L guide:
  https://dreamcast.wiki/Fast_SH4_Vertex_Processing (~32-vertex blocks).
- Store queues: 2 × 32 B burst to PVR TA without cache pollution — canonical
  geometry path. https://kos-docs.dreamcast.wiki/group__store__queues.html
- vs Gekko 485 MHz: community consensus ~2.5–4× slower; well above N64
  VR4300 93.75 MHz.
- **16 MB SDRAM**; KOS overhead ~2–4 MB practical. 32 MB RAM mod exists and
  KOS supports it (dca3 deliberately does not require it — hold that line).
  https://dreamcast.wiki/32MB_RAM_expansion

## AICA audio

- 64 HW channels (16/8-bit PCM, 4-bit Yamaha ADPCM), DSP, 2 MB sound RAM,
  ARM7 (weak — heavily wait-stated; don't plan CPU work there).
- KOS: `sfxmgr` (one-shots) + **`snd_stream`** (CPU-fed streaming; HW channel
  limit 2^16 samples ⇒ long audio must stream). `wav2adpcm` in KOS utils;
  dcaconv https://github.com/TapamN/dcaconv
- Modern ports: doom64-dc streams ADPCM music via libwav; dca3 transcodes all
  audio offline (`aud2adpcm.c`) and streams from disc. Software mixing on
  SH-4 generally avoided — but sm64-dc runs full N64 audio engine on SH-4 at
  full speed (precedent for rspsim stage A).

## Precedent ports (existence proofs)

- **dca3 — GTA III + Vice City** https://gitlab.com/skmp/dca3-game /
  https://www.dca3.net/ (skmp = Flycast author). re3+librw base; custom librw
  PVR driver (`rwdc.cpp` ~6.3k LOC): OP/PT/TR submission, TriStripper,
  **meshlets with 8-bit quantized vertices decoded by FTRV matrices**, OCR
  scratchpad, 32 B vertices via store queues, offline VQ (saved "~64 MB" of
  texture budget). Fits stock 16 MB. Streams from GD-ROM/GDEMU. 15–25 fps,
  playable start-to-finish. **Build-from-your-own-copy legal model** (repo =
  tools only; Colab CDI builder). Saves: 59–125+ VMU blocks — "dedicate a
  VMU".
- **sm64-dc** https://github.com/jnmartin84/sm64-dc (builder released
  2025-12-26): N64 decomp → GLdc, **full speed, native 480p, complete
  audio**, VMU saving, assets from user ROM, CDI via mkdcdisc + Colab.
- **doom64-dc** https://github.com/jnmartin84/doom64-dc: 60 fps, 16 dynamic
  lights + bump mapping, 8-bit paletted world textures, ADPCM streaming.
- **wipeout-dc** https://github.com/jnmartin84/wipeout-dc: 120+ fps, 24-bit
  color; v2 runs on stock KOS.
- OpenLara-dc (TR1, 60 fps); official Half-Life DC (complete, leaked);
  retail Quake 3. **No GameCube/TEV-based engine has ever been ported to DC**
  — we'd be first.

## Saves / VMU

- VMU: 128 KB flash, 200 usable × 512 B ≈ **100 KB user data** (homebrew
  unlock to 244 blocks possible, compat risk). KOS `vmu_pkg` APIs.
- Big-save model (dca3): dedicate a VMU. GDEMU/SD is NOT a save path.
- Modern options: VM2, 8BitMods VMU Pro (microSD, unlimited banks) — keep
  optional.

## Media / distribution

- **CDI** = self-booting CD-R (MIL-CD, 2 sessions). **GDI** = ODE/emulator
  only. **MIL-CD boots on VA0/VA1 consoles; late VA2 (post-Nov-2000)
  cannot** — check console revision before hardware phase.
- CD-R 700 MB; GD-ROM ~1 GB. Read speed: GD 12x CAV ~0.9–1.8 MB/s;
  **CD-R ~500 KB/s practical** — hot data on outer tracks.
- **mkdcdisc** (Simulant, ELF→CDI incl. CDDA): https://gitlab.com/simulant/mkdcdisc

## Dev / debug

- **Flycast** = de-facto dev emulator (dca3 lists it playable); **GDB server**
  (CMake `ENABLE_GDB_SERVER`, port 3263) works with gdb-multiarch/Ghidra.
- **lxdream-nitro**: GDB for SH4 *and* ARM7, MMIO tracing — closer-to-metal
  triage.
- Hardware: dcload-ip (BBA) / dcload-serial (coder's cable) + dc-tool; both
  rare hardware — CD-R iteration otherwise.
- dca3 pattern worth copying eventually: "koshle" KOS-HLE layer → run the DC
  build natively on PC for logic debugging.
