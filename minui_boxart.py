#!/usr/bin/env python3
"""Tiny MinUI box-art scraper for RG28xx (640x480).

Creates `Images/<GameNameWithExt>.png` (PNG, peaked at --width px, squared)
for every game entry under the MinUI Roms folder.

Handles MinUI packaging quirks learned the hard way:
- Directory-style PS games: `PS/Disney's Tarzan (Europe).cue/` (dir holding
  .cue/.bin tracks) needs art named `Disney's Tarzan (Europe).cue.png`.
- Libretro sometimes matches the wrong region (Metroid (Japan) is a wide
  scan, (USA) is square) -> OVERRIDES below pin the good art.
- Japan-only GBA games (Gyakuten Saiban) have no English GBA art ->
  OVERRIDES can point at another system's art (DS box front).
- Homebrew with no Libretro entry (Balatro GBA) -> MANUAL_URLS.
- mini-scraper only knows `.res`: it writes there and skips games with art
  there, and it would scan a visible `Images` folder as ROMs. So each system's
  art is renamed to `.res` while the scraper runs and moved back afterwards.

Usage:
    python3 minui_boxart.py [--roms <card>/Roms]
                            [--width 200]
                            [--systems PS,GBA] [--type boxart] [--force]
                            [--no-square-all] [--skip-scraper] [--migrate-only]

Runs from the Mac (needs internet and Node). --roms defaults to the Roms folder
beside the script's parent folder (<card>/Tools/ -> <card>/Roms) if there is
one, otherwise /Volumes/ROMS/Roms.

Existing `.res` art is moved to `Images` on every run. --migrate-only does just
that: no scraping, downloads or resizing.

Requires: python3, npx/node for the mini-scraper step (skipped gracefully
if absent), Pillow preferred for image work (falls back to macOS `sips`).
"""

import sys

sys.dont_write_bytecode = True  # keep __pycache__ off the card

import argparse
import os
import re
import shutil
import subprocess
import urllib.parse
import urllib.request

# Where art lives in each system folder. Needs a minui.elf that reads Images;
# LEGACY_ART_DIR is the stock location, which gets migrated.
ART_DIR = "Images"
LEGACY_ART_DIR = ".res"

# Folder name -> Libretro thumbnail set name (for gap-fill + overrides).
LIBRETRO_SYSTEMS = {
    "GBA": "Nintendo - Game Boy Advance",
    "GBC": "Nintendo - Game Boy Color",
    "GB": "Nintendo - Game Boy",
    "N64": "Nintendo - Nintendo 64",
    "PS": "Sony - PlayStation",
    "PSP": "Sony - PlayStation Portable",
    # MinUI stock folders keep the core suffix in parens.
    "Playstation Portable (PSPSA)": "Sony - PlayStation Portable",
    "Arcade (FBNEO)": "FBNeo - Arcade Games",
    "SFC": "Nintendo - Super Nintendo Entertainment System",
    "NES": "Nintendo - Nintendo Entertainment System",
    "NDS": "Nintendo - Nintendo DS",
    "DC": "Sega - Dreamcast",
    "MD": "Sega - Mega Drive - Genesis",
    "GG": "Sega - Game Gear",
}

# Fake "extensions" that are actually MinUI game directories, not files.
DIR_GAME_SUFFIXES = (".cue", ".m3u", ".chd", ".pbp")

# Preferred regions when several candidate arts match (USA first).
REGION_ORDER = ["USA", "Europe", "World", "Japan"]

# "SYS/game entry" -> Libretro art to use instead of the scraper's pick.
# Entry is the browser name (file OR directory name, extension included).
OVERRIDES = {
    # Scraper picks the wide (Japan) scan; (USA) is square.
    "GBA/Metroid - Zero Mission.gba": {
        "system": "Nintendo - Game Boy Advance",
        "art": "Metroid - Zero Mission (USA).png",
    },
    # GBA was Japan-only (Gyakuten Saiban, wide scans only); use the
    # official localized DS box front instead.
    "GBA/Phoenix Wright - Ace Attorney.gba": {
        "system": "Nintendo - Nintendo DS",
        "art": "Phoenix Wright - Ace Attorney (USA).png",
    },
}

# "SYS/game entry" -> direct image URL (homebrew etc. with no Libretro art).
MANUAL_URLS = {
    # Libretro has no PSP boxart for Puzzle Quest; this is the European PSP
    # cover from the game's Wikipedia article.
    "PSP/Puzzle Quest - Challenge of the Warlords.iso": (
        "https://upload.wikimedia.org/wikipedia/en/d/d6/Puzzle_quest_PSP.jpg"
    ),
    "GBA/Balatro v0.2.2.gba": (
        "https://external-content.duckduckgo.com/iu/?u="
        "https%3A%2F%2Fassets-prd.ignimgs.com%2F2024%2F01%2F24%2F"
        "balatro-button-1706119281330.jpg&f=1&nofb=1&ipt="
        "15d3b87fd67c02c63d116666413881358548f3e7c5642c5617060c35ebc824c0"
    ),
    # Renamed to human-readable title (was sfiii3.zip); FBNeo short names
    # never match Libretro, so pin the IGN button art directly.
    "Arcade (FBNEO)/Street Fighter III - 3rd Strike.zip": (
        "https://assets1.ignimgs.com/2019/01/18/"
        "stret-fighter-iii-third-strike-online---button-1547852624330.jpg"
        "?crop=1%3A1%2Csmart&format=jpg&auto=webp&quality=80"
    ),
}

try:
    from PIL import Image

    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def log(*a):
    print(*a, flush=True)


def norm(s):
    """Lowercase alnum-only, parens/tags stripped: for fuzzy title compare."""
    s = re.sub(r"\([^)]*\)", "", s)
    return re.sub(r"[^a-z0-9]", "", s.lower())


def region_rank(name):
    for i, r in enumerate(REGION_ORDER):
        if f"({r}" in name:
            return i
    return len(REGION_ORDER)


def game_entries(system_dir):
    """Top-level MinUI entries: files + game directories, minus dotfiles and art."""
    entries = []
    for name in sorted(os.listdir(system_dir)):
        if name.startswith(".") or name == ART_DIR:
            continue
        full = os.path.join(system_dir, name)
        if os.path.isfile(full) or os.path.isdir(full):
            if name.startswith("._"):
                continue
            entries.append(name)
    return entries


def art_path(system_dir, entry):
    return os.path.join(system_dir, ART_DIR, entry + ".png")


def migrate_art(roms, systems):
    """Move each system's .res contents into Images, then drop the empty .res.
    Never overwrites: a name already in Images stays, and the .res copy is left
    in place and logged."""
    for sys in systems:
        res = os.path.join(roms, sys, LEGACY_ART_DIR)
        images = os.path.join(roms, sys, ART_DIR)
        if not os.path.isdir(res):
            continue
        names = []
        for name in os.listdir(res):
            if name.startswith("._") or name == ".DS_Store":  # macOS litter
                os.remove(os.path.join(res, name))
            else:
                names.append(name)
        if names and not os.path.lexists(images):  # common case: rename whole
            os.rename(res, images)
            log(f"  migrated: {sys}/{LEGACY_ART_DIR} -> {ART_DIR} ({len(names)} files)")
            continue
        kept = 0
        for name in names:
            dst = os.path.join(images, name)
            if os.path.lexists(dst):
                kept += 1
                log(f"  KEPT in {LEGACY_ART_DIR}, {ART_DIR} already has it: {sys}/{name}")
                continue
            os.makedirs(images, exist_ok=True)
            os.rename(os.path.join(res, name), dst)
        if not kept:
            os.rmdir(res)
        if names:
            log(f"  migrated: {sys}/{LEGACY_ART_DIR} -> {ART_DIR} "
                f"({len(names) - kept} files, {kept} kept)")


def run_scraper(roms, width, art_type, systems):
    """Step 1: best-effort bulk scrape via mini-scraper (Libretro, no account)."""
    if not shutil.which("npx"):
        log("! npx/node not found, skipping mini-scraper step")
        return
    for sys in systems:
        sysdir = os.path.join(roms, sys)
        if not os.path.isdir(sysdir):
            continue
        res = os.path.join(sysdir, LEGACY_ART_DIR)
        images = os.path.join(sysdir, ART_DIR)
        if os.path.lexists(res):  # only left behind by a migrate conflict
            log(f"! {sys}/{LEGACY_ART_DIR} still exists, skipping mini-scraper for {sys}")
            continue
        cmd = [
            "npx", "-y", "@sinedied/mini-scraper", sysdir,
            "--width", str(width), "--output", "minui",
            "--type", art_type,
        ]
        log(f"$ {' '.join(cmd)}")
        if os.path.isdir(images):
            os.rename(images, res)  # see module docstring
        try:
            r = subprocess.run(cmd, capture_output=True, text=True)
        finally:
            migrate_art(roms, [sys])
        # Surface only the interesting lines.
        for line in (r.stdout + r.stderr).splitlines():
            if re.search(r"No art found|Scraped|not found|error", line, re.I):
                log("  " + line.strip())
        if r.returncode != 0:
            log(f"! scraper exit {r.returncode} for {sys} (continuing)")


def fetch(url, dst):
    req = urllib.request.Request(url, headers={"User-Agent": "minui-boxart/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp, open(dst, "wb") as f:
        shutil.copyfileobj(resp, f)


def process_image(src, dst, width):
    """Fit to `width` px wide (never upscale) and pad to a width^2 square."""
    if HAVE_PIL:
        img = Image.open(src)
        if img.width > width:
            h = round(img.height * width / img.width)
            img = img.resize((width, h), Image.LANCZOS)
        if img.size != (width, width):
            canvas = Image.new("RGBA", (width, width), (0, 0, 0, 0))
            canvas.paste(
                img.convert("RGBA"),
                ((width - img.width) // 2, (width - img.height) // 2),
            )
            img = canvas
        img.save(dst)
    else:  # macOS sips fallback
        shutil.copy(src, dst)
        subprocess.run(["sips", "-Z", str(width), dst],
                       check=True, capture_output=True)
        subprocess.run(["sips", "--padToHeightWidth", str(width), str(width),
                        "--padColor", "000000", dst],
                       check=True, capture_output=True)


def libretro_index(system):
    url = ("https://thumbnails.libretro.com/"
           + urllib.parse.quote(f"{system}/Named_Boxarts/"))
    html = urllib.request.urlopen(url, timeout=30).read().decode("utf-8", "ignore")
    return re.findall(r'href="([^"]+\.png)"', html)


def entry_region(name):
    for r in REGION_ORDER:
        if f"({r}" in name:
            return r
    return None


def pick_art(candidates, title):
    """Best candidate for `title`: exact core-name match, own region first,
    deprioritizing disc/demo/kiosk/Virtual Console variants."""
    want = norm(title)
    want_region = entry_region(title)
    exact = [c for c in candidates
             if norm(os.path.splitext(urllib.parse.unquote(c))[0]) == want]
    pool = exact or [c for c in candidates if want and want in
                     norm(os.path.splitext(urllib.parse.unquote(c))[0])]
    if not pool:
        return None

    def score(href):
        name = urllib.parse.unquote(href)
        cand_region = entry_region(name)
        if want_region and cand_region == want_region:
            rscore = -1
        else:
            rscore = region_rank(name)
        extra = 5 if re.search(
            r"disc|demo|kiosk|virtual console|promo|beta|prototype|rerelease",
            name, re.I) else 0
        return (rscore, extra, name)

    return sorted(pool, key=score)[0]


def title_of(entry, is_dir):
    base = entry
    if is_dir:  # "Game.cue" dir -> title is "Game" (+ keep region tags)
        for suf in DIR_GAME_SUFFIXES:
            if base.lower().endswith(suf.lower()):
                base = base[: -len(suf)]
                break
    else:
        base = os.path.splitext(base)[0]
    return base


def gap_fill(roms, width, systems, force):
    """Step 2: direct Libretro lookup for entries the scraper missed."""
    cache = {}
    for sys in systems:
        system_dir = os.path.join(roms, sys)
        libsys = LIBRETRO_SYSTEMS.get(sys)
        if not libsys:
            log(f"! no Libretro mapping for {sys}, skipping gap-fill")
            continue
        base_url = ("https://thumbnails.libretro.com/"
                    + urllib.parse.quote(f"{libsys}/Named_Boxarts/"))
        for i, entry in enumerate(game_entries(system_dir)):
            dst = art_path(system_dir, entry)
            if os.path.exists(dst) and not force:
                continue
            # Pinned/manual entries are handled in step 3; don't cry MISSING.
            if f"{sys}/{entry}" in OVERRIDES or f"{sys}/{entry}" in MANUAL_URLS:
                continue
            if sys not in cache:
                try:
                    cache[sys] = libretro_index(libsys)
                except Exception as e:
                    log(f"! index fetch failed for {libsys}: {e}")
                    cache[sys] = []
            title = title_of(entry, os.path.isdir(os.path.join(system_dir, entry)))
            hit = pick_art(cache[sys], title)
            if not hit:
                log(f"  MISSING: {sys}/{entry} (no Libretro candidate)")
                continue
            name = urllib.parse.unquote(hit)
            url = base_url + hit  # href from index is already percent-encoded
            tmp = f"/tmp/minui_{sys}_{i}.png"
            try:
                fetch(url, tmp)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                process_image(tmp, dst, width)
                log(f"  filled: {sys}/{entry} <- {name}")
            except Exception as e:
                log(f"  FAILED: {sys}/{entry} ({e})")


def apply_overrides(roms, width, systems, force):
    """Step 3: pinned art + manual URLs (idempotent unless --force)."""
    for key, spec in OVERRIDES.items():
        sys, entry = key.split("/", 1)
        if sys not in systems or not os.path.exists(os.path.join(roms, sys, entry)):
            continue
        dst = art_path(os.path.join(roms, sys), entry)
        if os.path.exists(dst) and not force:
            # Re-check squareness: fix wide leftovers from earlier runs.
            try:
                if HAVE_PIL and Image.open(dst).size == (width, width):
                    continue
            except Exception:
                pass
        url = ("https://thumbnails.libretro.com/"
               + urllib.parse.quote(f"{spec['system']}/Named_Boxarts/{spec['art']}"))
        tmp = "/tmp/minui_override.png"
        try:
            fetch(url, tmp)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            process_image(tmp, dst, width)
            log(f"  override: {key} <- {spec['art']}")
        except Exception as e:
            log(f"  OVERRIDE FAILED: {key} ({e})")
    for key, url in MANUAL_URLS.items():
        sys, entry = key.split("/", 1)
        if sys not in systems or not os.path.exists(os.path.join(roms, sys, entry)):
            continue
        dst = art_path(os.path.join(roms, sys), entry)
        if os.path.exists(dst) and not force:
            continue
        tmp = "/tmp/minui_manual." + url.split("?")[0].rsplit(".", 1)[-1][:3]
        try:
            fetch(url, tmp)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            process_image(tmp, dst, width)
            log(f"  manual: {key}")
        except Exception as e:
            log(f"  MANUAL FAILED: {key} ({e})")


def square_all(roms, width, systems):
    """Step 4: guarantee the invariant - every art file is width^2 square."""
    for sys in systems:
        res = os.path.join(roms, sys, ART_DIR)
        if not os.path.isdir(res):
            continue
        for name in sorted(os.listdir(res)):
            if not name.endswith(".png") or name.startswith("._"):
                continue
            p = os.path.join(res, name)
            try:
                if HAVE_PIL:
                    img = Image.open(p)
                    if img.size == (width, width):
                        continue
                    process_image(p, p, width)
                    log(f"  squared: {sys}/{name} {img.size} -> {(width, width)}")
                else:
                    out = subprocess.run(
                        ["sips", "-g", "pixelWidth", "-g", "pixelHeight", p],
                        capture_output=True, text=True)
                    if f"pixelWidth: {width}" in out.stdout \
                            and f"pixelHeight: {width}" in out.stdout:
                        continue
                    process_image(p, p, width)
                    log(f"  squared: {sys}/{name}")
            except Exception as e:
                log(f"  SQUARE FAILED: {sys}/{name} ({e})")


def cleanup(roms, systems):
    for sys in systems:
        res = os.path.join(roms, sys, ART_DIR)
        if not os.path.isdir(res):
            continue
        for name in os.listdir(res):
            if name.startswith("._"):
                os.remove(os.path.join(res, name))
                log(f"  cleaned AppleDouble: {sys}/{ART_DIR}/{name}")


def cleanup_orphans(roms, systems):
    """Remove art whose game entry no longer exists (e.g. ROMs that
    were converted from cue-dirs to PBP and renamed). Art is regenerable.
    Art for a game shelved in _Reserve/<sys>/ stays in place, ready for when
    the game moves back."""
    for sys in systems:
        system_dir = os.path.join(roms, sys)
        res = os.path.join(system_dir, ART_DIR)
        if not os.path.isdir(res):
            continue
        entries = set(game_entries(system_dir))
        for name in sorted(os.listdir(res)):
            if not name.endswith(".png") or name.startswith("._"):
                continue
            if name[: -len(".png")] not in entries:
                if os.path.exists(os.path.join(roms, "_Reserve", sys, name[: -len(".png")])):
                    continue
                os.remove(os.path.join(res, name))
                log(f"  removed orphan (no such game anymore): {sys}/{ART_DIR}/{name}")


def report(roms, systems):
    missing, total = [], 0
    for sys in systems:
        system_dir = os.path.join(roms, sys)
        if not os.path.isdir(system_dir):
            continue
        for entry in game_entries(system_dir):
            total += 1
            if not os.path.exists(art_path(system_dir, entry)):
                missing.append(f"{sys}/{entry}")
    log(f"\n{total - len(missing)}/{total} entries have art")
    for m in missing:
        log(f"  STILL MISSING: {m} -> add to OVERRIDES/MANUAL_URLS and re-run")


def default_roms():
    beside = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Roms")
    return beside if os.path.isdir(beside) else "/Volumes/ROMS/Roms"


def main():
    ap = argparse.ArgumentParser(description="MinUI tiny box-art scraper")
    ap.add_argument("--roms", default=default_roms())
    ap.add_argument("--width", type=int, default=200,
                    help="max art width px (200 for 640x480 RG28xx)")
    ap.add_argument("--systems", default="",
                    help="comma list to limit, e.g. PS,GBA (default: all)")
    ap.add_argument("--type", default="boxart",
                    choices=["boxart", "snap", "title", "box+snap", "box+title"])
    ap.add_argument("--force", action="store_true",
                    help="re-fetch even where art exists")
    ap.add_argument("--no-square-all", action="store_true")
    ap.add_argument("--no-cleanup-orphans", action="store_true",
                    help="keep art with no matching game entry")
    ap.add_argument("--skip-scraper", action="store_true")
    ap.add_argument("--migrate-only", action="store_true",
                    help="only move .res art to Images, then stop")
    a = ap.parse_args()

    systems = ([s.strip() for s in a.systems.split(",") if s.strip()]
               if a.systems else sorted(
                   d for d in os.listdir(a.roms)
                   if os.path.isdir(os.path.join(a.roms, d))
                   and not d.startswith((".", "_"))))
    log(f"systems: {systems}  width: {a.width}")
    migrate_art(a.roms, systems)
    if a.migrate_only:
        return
    if not a.skip_scraper:
        run_scraper(a.roms, a.width, a.type, systems)
    gap_fill(a.roms, a.width, systems, a.force)
    apply_overrides(a.roms, a.width, systems, a.force)
    if not a.no_square_all:
        square_all(a.roms, a.width, systems)
    if not a.no_cleanup_orphans:
        cleanup_orphans(a.roms, systems)
    cleanup(a.roms, systems)
    report(a.roms, systems)


if __name__ == "__main__":
    sys.exit(main())
