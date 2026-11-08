#!/usr/bin/env python3
"""Regenerate the Simplified-Chinese font subset and glyph ranges.

The zh-CN UI text lives in the sources as UTF-8 literals, so the character set
the UI needs can be derived from the sources.  This script

  1. collects every CJK character used in the repository's sources,
  2. asks ImGui for its 2500 common Chinese characters (parsed from
     core/src/imgui/imgui_draw.cpp) so user-entered names also render,
  3. instances the Noto Sans SC variable font at weight 400 and subsets it to
     those characters, and
  4. writes core/src/gui/cjk_glyphs.h with the generated ImWchar ranges.

Run it after changing any translated string, then commit the two outputs:

    python scripts/zh/gen_cjk_glyphs.py

The font is OFL-licensed; see root/res/licenses/NotoSansSC-OFL.txt.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import urllib.request

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FONT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
FONT_CACHE = os.path.join(os.path.dirname(__file__), ".fonts")
FONT_OUT = os.path.join(REPO, "root", "res", "fonts", "NotoSansSC-Regular-subset.ttf")
HEADER_OUT = os.path.join(REPO, "core", "src", "gui", "cjk_glyphs.h")
IMGUI_DRAW = os.path.join(REPO, "core", "src", "imgui", "imgui_draw.cpp")

SKIP_DIRS = {"deps", "imgui", ".git", "build", "root"}
CJK_RANGES = [
    (0x3400, 0x4DBF),   # CJK Extension A
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs
    (0x3000, 0x303F),   # CJK Symbols and Punctuation
    (0xFF00, 0xFFEF),   # Halfwidth and Fullwidth Forms
]
EXTRA_PUNCT = "　，。、；：？！“”‘’（）【】《》〈〉「」『』…—～·"


def is_cjk(ch: str) -> bool:
    o = ord(ch)
    return any(a <= o <= b for a, b in CJK_RANGES)


def covered_by_base_font(ch: str) -> bool:
    """Roboto already supplies ASCII/Latin-1, Cyrillic and a few singletons."""
    o = ord(ch)
    if o <= 0x00FF or 0x0370 <= o <= 0x03FF or 0x0400 <= o <= 0x04FF:
        return True
    return o in (0x2015, 0x2030, 0x20AC)


def collect_used_chars() -> set[str]:
    chars: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(REPO):
        rel = os.path.relpath(dirpath, REPO).replace("\\", "/")
        parts = rel.split("/")
        if any(p in SKIP_DIRS for p in parts) or (rel.startswith(".") and rel != "."):
            dirnames[:] = []
            continue
        for fn in filenames:
            # Only sources that end up in the UI; documentation and data files
            # would inflate the glyph atlas with characters nobody draws.
            if not fn.endswith((".cpp", ".h", ".hpp", ".c")):
                continue
            try:
                text = open(os.path.join(dirpath, fn), encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for ch in text:
                if is_cjk(ch):
                    chars.add(ch)
    chars.update(EXTRA_PUNCT)
    return chars


def parse_imgui_common() -> set[str]:
    """Reproduce ImFontAtlas::GetGlyphRangesChineseSimplifiedCommon()."""
    text = open(IMGUI_DRAW, encoding="utf-8", errors="ignore").read()
    m = re.search(r"accumulative_offsets_from_0x4E00\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("cannot locate ImGui's common-Chinese table")
    cp = 0x4E00
    chars = set()
    for off in (int(x) for x in re.findall(r"-?\d+", m.group(1))):
        cp += off
        chars.add(chr(cp))
    return chars


def ranges_from_chars(chars: set[str]) -> list[tuple[int, int]]:
    cps = sorted(ord(c) for c in chars if ord(c) <= 0xFFFF)
    out = []
    i = 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        out.append((cps[i], cps[j]))
        i = j + 1
    return out


def emit_ranges(name: str, chars: set[str]) -> str:
    lines, cur = [], []
    for a, b in ranges_from_chars(chars):
        cur.append("0x%04X, 0x%04X," % (a, b))
        if len(cur) == 4:
            lines.append("    " + " ".join(cur))
            cur = []
    if cur:
        lines.append("    " + " ".join(cur))
    return "static const ImWchar %s[] = {\n%s\n    0\n};\n" % (name, "\n".join(lines))


def ensure_static_font() -> str:
    os.makedirs(FONT_CACHE, exist_ok=True)
    variable = os.path.join(FONT_CACHE, "NotoSansSC-VF.ttf")
    static = os.path.join(FONT_CACHE, "NotoSansSC-Regular.ttf")
    if not os.path.exists(variable):
        print(f"downloading {FONT_URL}")
        urllib.request.urlretrieve(FONT_URL, variable)
    if not os.path.exists(static):
        from fontTools.ttLib import TTFont
        from fontTools.varLib.instancer import instantiateVariableFont

        print("instancing variable font at wght=400")
        font = TTFont(variable)
        instantiateVariableFont(font, {"wght": 400}, inplace=False).save(static)
    return static


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="static Regular TTF to subset (default: cached Noto Sans SC)")
    args = ap.parse_args()

    used = {c for c in collect_used_chars() if not covered_by_base_font(c)}
    common = {c for c in parse_imgui_common() if not covered_by_base_font(c)}
    all_chars = used | common
    print(f"UI characters: {len(used)}, ImGui common characters: {len(common)}, total: {len(all_chars)}")

    font = args.font or ensure_static_font()
    chars_file = os.path.join(FONT_CACHE, "subset_chars.txt")
    with open(chars_file, "w", encoding="utf-8") as f:
        f.write("".join(sorted(all_chars)))

    os.makedirs(os.path.dirname(FONT_OUT), exist_ok=True)
    subprocess.run([sys.executable, "-m", "fontTools.subset", font,
                    f"--text-file={chars_file}", f"--output-file={FONT_OUT}",
                    "--layout-features=*", "--name-IDs=*", "--name-languages=*",
                    "--name-legacy", "--notdef-glyph", "--notdef-outline",
                    "--recommended-glyphs", "--glyph-names", "--symbol-cmap",
                    "--legacy-cmap"], check=True)

    header = ["// Generated by scripts/zh/gen_cjk_glyphs.py -- do not edit by hand.",
              "#pragma once", "#include <imgui.h>", "",
              "// Characters used by the zh-CN UI text (all platforms).",
              emit_ranges("cjkUiRanges", used), "",
              "// UI characters plus the 2500 common Chinese characters ImGui ships,",
              "// so user-entered text (bookmark names, ...) also renders on desktop.",
              emit_ranges("cjkCommonRanges", all_chars)]
    open(HEADER_OUT, "w", encoding="utf-8", newline="\n").write("\n".join(header))
    print(f"wrote {FONT_OUT} ({os.path.getsize(FONT_OUT) / 1024:.1f} KiB)")
    print(f"wrote {HEADER_OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
