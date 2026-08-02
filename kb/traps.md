# Traps already paid for — do not re-discover these

Each one cost real debugging time. **Read this before touching the build, the
harness, or the prelude.** These are mechanical gotchas, not design decisions —
for those see `kb/closed.md`.

## Compile / link

- **`fsqrt` collision.** KOS `dc/fmath.h:109` defines
  `static inline float fsqrt(float)`; the decomp's `math64.h:34`
  `#define fsqrt(x) sqrtf(x)` rewrites KOS's *definition* into a static `sqrtf`
  that collides with newlib.
- **POSIX `link()` vs the decomp's `typedef struct link_ link`**, arriving via
  `<stdio.h>` → `<sys/stdio.h>` → `<unistd.h>`. A blanket `-Dlink=` does **not**
  work — it renames both sides. `dc/include/dc_prelude.h` renames only the POSIX
  declaration, then gives the identifier back.
- **`-fno-builtin` breaks the link** — see `kb/closed.md`.
- **3900 object paths exceed `execve`'s `ARG_MAX`.** The Makefile uses make's
  `$(file …)` to build a linker response file.
- **`char` is SIGNED by default** on this toolchain build, so `-fsigned-char`
  is belt-and-braces, not load-bearing.

## Harness / emulator

- **Guest `scif_flush()` permanently kills the Flycast console. Never call it.**
  KOS's flush clears TEND and spins; Flycast never re-raises TEND on an idle TX
  FIFO; KOS latches `serial_enabled = 0`; a later crash then prints **nothing**.
  Bisected across 7 guest variants — raising baud is fine, the flush is the
  killer.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` / capital-A
  `Assertion "x" failed`. The documented lowercase regex never matched, so a
  failed `assert()` only ever surfaced as a timeout.
- **mkdcdisc padding**: default 740,083,145 B / 15.6 s vs `-N` 1,783,337 B /
  0.021 s. Use `-N` for every emulator run; `DC_CDI_PAD=1` only for burns and
  read-speed-realistic timing.

## Docker / SDK image

- **`bash -lc` inside the SDK image** re-runs `/etc/profile`, which drops
  `/opt/toolchains/dc/sh-elf/bin` from PATH. `sh-elf-addr2line` then vanishes
  and every address silently symbolises to `??`. **Use `bash -c`.**
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **Host has no BuildKit** — `DOCKER_BUILDKIT=0`, never pass `--progress`.
  `--platform linux/arm64` is not optional; without it an amd64 pull drops the
  build into qemu.
- **Do not rebuild `opencrossing-dc:sdk`** — ~27 min cold. It is already in the
  local Docker daemon.

## Agent hygiene

- **Agents must not run git.** The main thread commits.
- **Always give absolute paths in scripts** — agents run from varying cwds.
