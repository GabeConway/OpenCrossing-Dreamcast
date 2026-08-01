"""Parse ``pc/src/pc_assets.c`` into an ordered list of asset references.

``pc_assets.c`` is generated (``pc/tools/gen_runtime_assets.py``) and is the
*only* thing that reads the decompressed ``foresta.rel`` / ``main.dol`` blobs
(verified: ``g_rel_data`` / ``g_dol_data`` are file-static there and
``pc_disc_extract_rel`` / ``pc_disc_extract_dol`` have no other callers).

Two reference kinds exist and both are recovered here:

1. the central ``static const PCAsset s_assets[]`` table, and
2. 769 generated ``_pc_load_src_*()`` functions living in ``src/``, each
   holding one or more ``pc_load_asset(...)`` calls.

``pc_assets_init()`` walks the table first, then calls the 769 functions in a
fixed textual order, and each function's calls run in *its* textual order. The
whole boot-time asset load is therefore a **deterministic, statically known
sequence** -- which is what lets ``dcasset pack`` lay the pack out in access
order and let the Dreamcast stream it forward off CD-R without seeking.

Nothing here reads a disc image; this is pure source analysis.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

SRC_REL, SRC_DOL, SRC_NONE = 0, 1, 2
SWAP_NONE, SWAP_U16, SWAP_VTX, SWAP_U32 = 0, 1, 2, 3

SWAP_NAMES = {SWAP_NONE: "none", SWAP_U16: "u16", SWAP_VTX: "vtx",
              SWAP_U32: "u32"}
SOURCE_NAMES = {SRC_REL: "foresta.rel", SRC_DOL: "main.dol"}

# Sizes of the two blobs on GAFE01 rev 0 (see tools/dcasset/README.md).
REL_BLOB_SIZE = 15_640_056
DOL_BLOB_SIZE = 918_720
BLOB_SIZE = {SRC_REL: REL_BLOB_SIZE, SRC_DOL: DOL_BLOB_SIZE}

# Generated call sites carry inline /* SRC_DOL */-style comments between the
# arguments, so every separator has to tolerate them. Missing that costs real
# call sites (it silently dropped one in JFWSystem.cpp).
_C = r'(?:\s|/\*.*?\*/)*'
_NUM = r'(0x[0-9A-Fa-f]+|\d+)'
_ARGS = (
    _C + _NUM + _C + r',' + _C + _NUM + _C + r',' + _C + r'(\d+)'
    + _C + r',' + _C + r'(\d+)' + _C
)
CALL_RE = re.compile(
    r'pc_load_asset' + _C + r'\(' + _C + r'(?:"(?:[^"\\]|\\.)*"|NULL)'
    + _C + r',[^,]+,' + _ARGS + r'\)', re.S)
TABLE_RE = re.compile(
    r'\{' + _C + r'(?:"(?:[^"\\]|\\.)*"|NULL)' + _C + r',[^,]+,'
    + _ARGS + r'\}', re.S)

_OCC_RE = re.compile(r'pc_load_asset\s*\(')
_PROTO_RE = re.compile(r'pc_load_asset\s*\(\s*const\s+char\s*\*')
_INIT_CALL_RE = re.compile(r'^[ \t]*(_pc_load_\w+)\(\);', re.M)
_DEF_RE = re.compile(r'\bvoid\s+(_pc_load_\w+)\s*\(void\)\s*\{')

TABLE_MARKER = "static const PCAsset s_assets[] = {"
INIT_MARKER = "void pc_assets_init(void) {"


@dataclass(frozen=True)
class AssetRef:
    """One ``pc_load_asset`` reference, in boot load order."""
    seq: int          # global load-order index, 0-based
    src: int          # SRC_REL / SRC_DOL
    off: int          # byte offset inside the blob
    size: int         # bytes copied
    swap: int         # SWAP_* applied to the destination after the copy
    origin: str       # "table" or the _pc_load_* function name

    @property
    def end(self) -> int:
        return self.off + self.size


def _brace_body(text: str, open_idx: int) -> str:
    """Return the text between the brace at ``open_idx`` and its match."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
        i += 1
    return text[open_idx + 1:]


def scan(repo: Path) -> tuple[list[AssetRef], dict]:
    """Scan ``repo`` and return (ordered refs, reconciliation stats).

    The reconciliation stats are the same accounting ``relmap`` prints: every
    textual ``pc_load_asset`` occurrence under ``src/`` must be either a
    prototype or a parsed call, else the span set is only a lower bound.
    """
    repo = Path(repo).resolve()
    assets_c = repo / "pc" / "src" / "pc_assets.c"
    if not assets_c.exists():
        raise FileNotFoundError(str(assets_c))
    txt = assets_c.read_text(errors="replace")

    refs: list[AssetRef] = []
    seq = 0

    # ---- 1. the central table, in table order ----------------------------
    table_entries = 0
    if TABLE_MARKER in txt:
        i = txt.index(TABLE_MARKER)
        j = txt.index("\n};", i)
        for m in TABLE_RE.finditer(txt[i:j]):
            size, off = int(m.group(1), 0), int(m.group(2), 0)
            src, swap = int(m.group(3)), int(m.group(4))
            table_entries += 1
            if src in (SRC_REL, SRC_DOL):
                refs.append(AssetRef(seq, src, off, size, swap, "table"))
                seq += 1

    # ---- 2. index every generated loader function found under src/ -------
    bodies: dict[str, tuple[str, str]] = {}   # name -> (file, body text)
    occurrences = prototypes = src_matched = 0
    per_file_calls: dict[str, int] = {}
    # calls that are NOT inside a generated _pc_load_*() function: they run at
    # some unknown point, so they cannot be placed in load order. There is
    # exactly one on GAFE01 (src/furniture/ac_radio_test.c, rom_src=SRC_NONE),
    # but the scan must not assume that.
    stray: list[tuple[str, int, int, int, int]] = []
    for root, _dirs, fnames in os.walk(repo / "src"):
        for fn in sorted(fnames):
            p = Path(root) / fn
            try:
                t = p.read_text(errors="replace")
            except OSError:
                continue
            if "pc_load_asset" not in t:
                continue
            occurrences += len(_OCC_RE.findall(t))
            prototypes += len(_PROTO_RE.findall(t))
            per_file_calls[str(p)] = len(CALL_RE.findall(t))
            spans_in_func: list[tuple[int, int]] = []
            for m in _DEF_RE.finditer(t):
                open_idx = t.index('{', m.end() - 1)
                body = _brace_body(t, open_idx)
                bodies[m.group(1)] = (str(p), body)
                spans_in_func.append((open_idx, open_idx + len(body) + 2))
            for m in CALL_RE.finditer(t):
                if any(a <= m.start() < b for a, b in spans_in_func):
                    continue
                stray.append((f"{p}:{t[:m.start()].count(chr(10)) + 1}",
                              int(m.group(2), 0), int(m.group(1), 0),
                              int(m.group(3)), int(m.group(4))))

    # ---- 3. replay pc_assets_init's call order ---------------------------
    init_calls: list[str] = []
    if INIT_MARKER in txt:
        k = txt.index(INIT_MARKER)
        init_calls = _INIT_CALL_RE.findall(_brace_body(txt, txt.index('{', k)))

    missing = [c for c in init_calls if c not in bodies]
    uncalled = sorted(set(bodies) - set(init_calls))
    for name in init_calls:
        entry = bodies.get(name)
        if entry is None:
            continue
        for m in CALL_RE.finditer(entry[1]):
            size, off = int(m.group(1), 0), int(m.group(2), 0)
            src, swap = int(m.group(3)), int(m.group(4))
            src_matched += 1
            if src in (SRC_REL, SRC_DOL):
                refs.append(AssetRef(seq, src, off, size, swap, name))
                seq += 1

    # Functions that exist but are never called still count as parsed calls
    # for the reconciliation (relmap parity), and their bytes are *not* in the
    # pack -- flagged loudly if it ever happens.
    for name in uncalled:
        src_matched += len(CALL_RE.findall(bodies[name][1]))

    # Stray calls land at the end of the sequence: their real load order is
    # unknown, so they get the worst-case position rather than a guessed one.
    stray_packed = 0
    for where, off, size, src, swap in stray:
        src_matched += 1
        if src in (SRC_REL, SRC_DOL):
            refs.append(AssetRef(seq, src, off, size, swap, where))
            seq += 1
            stray_packed += 1

    total_src_calls = sum(per_file_calls.values())
    stats = {
        "table_entries": table_entries,
        "init_function_calls": len(init_calls),
        "loader_functions_defined": len(bodies),
        "loader_functions_missing_def": missing,
        "loader_functions_never_called": uncalled,
        "src_occurrences": occurrences,
        "src_prototypes": prototypes,
        "src_parsed_calls": src_matched,
        "src_calls_regex_total": total_src_calls,
        "src_unaccounted": occurrences - prototypes - src_matched,
        "calls_outside_loader_functions": [
            {"where": w, "off": o, "size": s, "src": c, "swap": sw}
            for w, o, s, c, sw in stray
        ],
        "calls_outside_packed": stray_packed,
        "refs_ordered": len(refs),
    }
    return refs, stats


def merge_spans(refs: list[AssetRef], src: int) -> list[tuple[int, int]]:
    """Merge one source's refs into disjoint [start, end) spans."""
    rs = sorted((r.off, r.end) for r in refs if r.src == src)
    merged: list[list[int]] = []
    for a, b in rs:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return [(a, b) for a, b in merged]
