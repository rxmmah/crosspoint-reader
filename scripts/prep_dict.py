#!/usr/bin/env python3
"""
prep_dict.py — Offline StarDict dictionary pre-processor for CrossPoint Reader.

Replicates the on-device preparation steps performed by DictPrepareActivity
so dictionaries can be prepared on the host machine before copying to the SD card.

Steps performed (matching DictPrepareActivity.detectSteps()):
  1. Extract .dict.dz -> .dict   (if .dict.dz present and .dict absent)
  2. Extract .syn.dz  -> .syn    (if .syn.dz  present and .syn  absent)
  3. Generate .idx.oft            (if .idx present and .idx.oft absent)
  4. Generate .syn.oft            (if .syn present (or will exist) and .syn.oft absent)

Usage:
    python3 scripts/prep_dict.py /path/to/my-dictionary

Output:
    Processed files are written to dictionaries/<stem>/ in the project root.
    The dictionaries/ folder is gitignored -- safe to write to.
    Copy the output folder to the SD card dictionary folder before use.
"""

import argparse
import gzip
import shutil
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# .oft constants — matches StarDict cache format and DictPrepareActivity
# ---------------------------------------------------------------------------

_OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(_OFT_HEADER) == 38
_STRIDE = 32


def _build_oft(data: bytes, skip_bytes_after_null: int) -> bytes:
    """Build .oft index bytes for .idx (skip=8) or .syn (skip=4)."""
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(data):
        null = data.index(b"\x00", pos)
        pos = null + 1 + skip_bytes_after_null
        entry_count += 1
        if entry_count % _STRIDE == 0:
            offsets.append(pos)
    result = _OFT_HEADER
    for off in offsets:
        result += struct.pack("<I", off)
    return result


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _find_stem(folder: Path) -> str:
    """Return the StarDict stem by locating the .ifo file in the folder."""
    ifo_files = list(folder.glob("*.ifo"))
    if not ifo_files:
        print(f"ERROR: no .ifo file found in {folder}", file=sys.stderr)
        sys.exit(1)
    if len(ifo_files) > 1:
        print(f"ERROR: multiple .ifo files in {folder} -- cannot determine stem", file=sys.stderr)
        sys.exit(1)
    return ifo_files[0].stem


def _decompress(src: Path, dst: Path) -> None:
    """Gzip-decompress src into dst."""
    with gzip.open(src, "rb") as f_in, open(dst, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)


# ---------------------------------------------------------------------------
# Main logic
# ---------------------------------------------------------------------------

def prep(source_folder: Path) -> None:
    if not source_folder.is_dir():
        print(f"ERROR: not a directory: {source_folder}", file=sys.stderr)
        sys.exit(1)

    stem = _find_stem(source_folder)

    project_root = Path(__file__).parent.parent
    out_dir = project_root / "dictionaries" / stem

    # Copy source files into output directory.
    if out_dir.exists():
        shutil.rmtree(out_dir)
    shutil.copytree(source_folder, out_dir)
    print(f"Copied {source_folder} -> {out_dir}")

    steps_run = 0

    # Step 1: Extract .dict.dz -> .dict
    dz    = out_dir / f"{stem}.dict.dz"
    dict_ = out_dir / f"{stem}.dict"
    if dz.exists() and not dict_.exists():
        print(f"  Extracting {dz.name} -> {dict_.name} ...", end=" ", flush=True)
        _decompress(dz, dict_)
        print(f"{dict_.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 2: Extract .syn.dz -> .syn
    syn_dz = out_dir / f"{stem}.syn.dz"
    syn    = out_dir / f"{stem}.syn"
    if syn_dz.exists() and not syn.exists():
        print(f"  Extracting {syn_dz.name} -> {syn.name} ...", end=" ", flush=True)
        _decompress(syn_dz, syn)
        print(f"{syn.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 3: Generate .idx.oft
    idx     = out_dir / f"{stem}.idx"
    idx_oft = out_dir / f"{stem}.idx.oft"
    if idx.exists() and not idx_oft.exists():
        print(f"  Generating {idx_oft.name} ...", end=" ", flush=True)
        idx_oft.write_bytes(_build_oft(idx.read_bytes(), skip_bytes_after_null=8))
        print(f"{idx_oft.stat().st_size} bytes")
        steps_run += 1

    # Step 4: Generate .syn.oft (.syn may have just been created in step 2)
    syn_oft = out_dir / f"{stem}.syn.oft"
    if syn.exists() and not syn_oft.exists():
        print(f"  Generating {syn_oft.name} ...", end=" ", flush=True)
        syn_oft.write_bytes(_build_oft(syn.read_bytes(), skip_bytes_after_null=4))
        print(f"{syn_oft.stat().st_size} bytes")
        steps_run += 1

    if steps_run == 0:
        print("  No preparation steps needed -- dictionary already processed.")
    else:
        print(f"  Done. {steps_run} step(s) completed.")
    print(f"  Output: {out_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Offline StarDict dictionary pre-processor for CrossPoint Reader."
    )
    parser.add_argument("folder", help="Path to the dictionary folder to process")
    args = parser.parse_args()
    prep(Path(args.folder))


if __name__ == "__main__":
    main()
