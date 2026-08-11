#!/usr/bin/env python3
"""Generate all C sprite/asset arrays into a destination dir.

Usage: gen_assets.py <out_dir> <pet_sprite_dir>

The pet sprites always emit with a stable "pet" prefix, so swapping the
character is just pointing <pet_sprite_dir> at a different PMD sprite folder
(e.g. assets/raichu -> assets/pikachu) — no code changes. The field, HUD,
and font generators are character-independent.

Outputs (into <out_dir>): pet_sprites.{c,h}, field_bg.{c,h}, hud_box.{c,h},
pixel_font.{c,h}. Idempotent: skips regeneration when inputs are older than
existing outputs, so incremental builds stay fast.
"""
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def newer_than_outputs(inputs, outputs):
    if not all(o.exists() for o in outputs):
        return True
    newest_in = max((p.stat().st_mtime for p in inputs if p.exists()), default=0)
    oldest_out = min(o.stat().st_mtime for o in outputs)
    return newest_in > oldest_out


def run(script, *args):
    subprocess.run([sys.executable, str(TOOLS / script), *map(str, args)], check=True)


def main():
    out_dir = Path(sys.argv[1])
    pet_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    # Pet sprites (character-specific).
    pet_inputs = sorted(pet_dir.glob("*")) + [TOOLS / "convert_sprites.py"]
    pet_outputs = [out_dir / "pet_sprites.c", out_dir / "pet_sprites.h"]
    if newer_than_outputs(pet_inputs, pet_outputs):
        run("convert_sprites.py", pet_dir, out_dir, "pet")

    # Character-independent assets.
    for script, stem in (
        ("make_field_bg.py", "field_bg"),
        ("make_hud.py", "hud_box"),
        ("make_pixel_font.py", "pixel_font"),
    ):
        outputs = [out_dir / f"{stem}.c", out_dir / f"{stem}.h"]
        if newer_than_outputs([TOOLS / script], outputs):
            run(script, out_dir)


if __name__ == "__main__":
    main()
